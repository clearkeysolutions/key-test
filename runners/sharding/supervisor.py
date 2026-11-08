"""Resumable, sharded execution of a plan set against ckt_worker (SPEC §8).

Guarantees
* Idempotent: a case is executed until exactly one terminal result exists for
  it; completed cases are never re-run, and result files are append-only.
* Reader faults are outcomes: a crash or hang while the production reader is
  running (progress marker SCAN without SCANNED) is recorded as a completed
  failed case (READER_CRASH / READER_TIMEOUT); one reproduction attempt runs
  in a fresh process and is logged separately in retries/, never replacing
  the counted outcome.
* Harness faults are not outcomes: a crash/hang outside the reader call, or a
  HARNESS_ERROR line, means the case is not completed; it is re-run once, and
  a second fault on the same case halts the campaign.
* Invalid generation halts the campaign immediately.
* Sealing: a finished shard's results/traces are gzipped, hashed, recorded in
  manifests/<set>.jsonl (append-only) and made read-only.
"""
from __future__ import annotations

import gzip
import hashlib
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

from generator.common import ROOT, file_sha256, load_yaml, worker_path


class Halt(RuntimeError):
    """Stop the campaign and report (SPEC §8: never silently skip)."""


@dataclass
class Totals:
    lock: threading.Lock = field(default_factory=threading.Lock)
    done: int = 0
    success: int = 0
    wrong: int = 0
    reader_faults: int = 0
    harness_faults: int = 0
    started: float = field(default_factory=time.time)


def read_jsonl(path: Path):
    """Valid JSON objects from a (possibly partially written) JSONL file."""
    if not path.exists():
        return
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                continue  # a torn final line from a crash: ignored, logged by audit


def append_jsonl(path: Path, obj: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a") as fh:
        fh.write(json.dumps(obj, separators=(",", ":")) + "\n")
        fh.flush()
        os.fsync(fh.fileno())


def ensure_newline(path: Path) -> None:
    """Terminate a torn final line so appends start on a fresh line (the torn line stays, unparsed)."""
    if path.exists() and path.stat().st_size:
        with open(path, "rb+") as fh:
            fh.seek(-1, os.SEEK_END)
            if fh.read(1) != b"\n":
                fh.write(b"\n")


class Supervisor:
    def __init__(self, data_dir: Path, plan_set: str, workers: int | None = None, worker: Path | None = None,
                 max_hours: float | None = None, log=print):
        self.cfg = load_yaml("campaign.yaml")
        self.data = data_dir
        self.set = plan_set
        self.workers = workers or (os.cpu_count() if self.cfg["workers"] == "auto" else int(self.cfg["workers"]))
        self.worker = Path(worker or worker_path())
        self.worker_sha = file_sha256(self.worker)
        self.max_hours = max_hours or self.cfg["max_hours"]
        self.log = log
        self.plan_dir = data_dir / "plans" / plan_set
        self.res_dir = data_dir / "results" / plan_set
        self.tr_dir = data_dir / "traces" / plan_set
        self.prog_dir = data_dir / "progress" / plan_set
        self.img_dir = data_dir / "images" / plan_set
        self.manifest_path = data_dir / "manifests" / f"{plan_set}.jsonl"
        self.infra_path = data_dir / "infra" / f"{plan_set}.jsonl"
        self.retry_path = data_dir / "retries" / f"{plan_set}.jsonl"
        for d in (self.res_dir, self.tr_dir, self.prog_dir, self.img_dir):
            d.mkdir(parents=True, exist_ok=True)
        self.totals = Totals()
        self.stop = threading.Event()
        self.halt_reason: str | None = None
        self.harness_fault_count: dict[str, int] = {}
        self.fault_lock = threading.Lock()

    # ------------------------------------------------------------------ helpers
    def sealed(self) -> dict[int, dict]:
        return {r["shard"]: r for r in read_jsonl(self.manifest_path)}

    def plan_manifest(self) -> dict:
        return json.loads((self.plan_dir / "manifest.json").read_text())

    def shard_paths(self, n: int):
        stem = f"shard-{n:05d}"
        return (self.plan_dir / f"{stem}.jsonl.gz", self.res_dir / f"{stem}.jsonl", self.tr_dir / f"{stem}.jsonl", self.prog_dir / f"{stem}.log", stem)

    def infra(self, **ev) -> None:
        ev.update({"time": time.time(), "host": socket.gethostname()})
        append_jsonl(self.infra_path, ev)

    def harness_fault(self, case_id: str, why: str) -> None:
        with self.fault_lock:
            n = self.harness_fault_count.get(case_id, 0) + 1
            self.harness_fault_count[case_id] = n
        with self.totals.lock:
            self.totals.harness_faults += 1
        self.infra(kind="harness_fault", case_id=case_id, reason=why, occurrence=n)
        if n >= 2:
            self.halt(f"repeated harness fault on {case_id}: {why}")

    def halt(self, why: str) -> None:
        if self.halt_reason is None:
            self.halt_reason = why
            self.log(f"\n*** HALT: {why}\n")
        self.stop.set()

    def budget_ok(self) -> bool:
        if (time.time() - self.totals.started) / 3600 > self.max_hours:
            self.log(f"budget ceiling reached ({self.max_hours} h); stopping cleanly (resume to continue)")
            self.stop.set()
            return False
        free_gb = shutil.disk_usage(self.data).free / 1e9
        if free_gb < self.cfg["min_free_disk_gb"]:
            self.halt(f"free disk {free_gb:.1f} GB below min_free_disk_gb={self.cfg['min_free_disk_gb']}")
            return False
        return True

    # ------------------------------------------------------------------ attribution
    @staticmethod
    def last_open_markers(progress: Path) -> tuple[str | None, str | None]:
        """(case_id in flight, 'scan' if inside the reader call else 'harness')."""
        if not progress.exists():
            return None, None
        open_case, phase = None, None
        with open(progress, "r", errors="replace") as fh:
            for line in fh:
                parts = line.strip().split(" ", 1)
                if len(parts) != 2:
                    continue
                tag, ident = parts
                if tag == "BEGIN":
                    open_case, phase = ident, "harness"
                elif tag == "SCAN":
                    phase = "scan"
                elif tag == "SCANNED":
                    phase = "harness"
                elif tag == "END":
                    open_case, phase = None, None
        return open_case, phase

    def reader_fault_result(self, case: dict, kind: str, detail: str) -> dict:
        return {"status": "COMPLETED", "case_id": case["case_id"], "cohort": case.get("cohort"), "stratum": case.get("stratum"),
                "profile": case.get("profile"), "mode": case.get("mode"), "sev_bin": case.get("sev_bin"), "factor": case.get("factor"),
                "severity": case.get("severity"), "payload_family": case.get("payload_family"), "expected_outcome": case.get("expected"),
                "observed_outcome": kind, "success": False, "wrong_payload": False, "failure_reason": f"READER_{kind}",
                "neg_kind": case.get("neg_kind", "none"), "camera_model": case.get("device"), "seed": case.get("seed"),
                "frame_count": 0, "elapsed_ms": None, "supervisor_detail": detail, "versions": "supervisor-synthesized"}

    def reproduce(self, case: dict, kind: str) -> None:
        """One isolated reproduction attempt of a reader fault; logged in retries/, outcome unchanged."""
        tmp = self.prog_dir / f"repro-{case['case_id']}.json"
        tmp.write_text(json.dumps(case) + "\n")
        t0 = time.time()
        try:
            p = subprocess.run([str(self.worker), "--repro", str(tmp)], capture_output=True, text=True,
                               timeout=self.cfg["case_timeout_s"] * max(1, int(case.get("frames", 1))))
            outcome = {"returncode": p.returncode, "stdout_tail": p.stdout[-400:], "stderr_tail": p.stderr[-400:]}
        except subprocess.TimeoutExpired:
            outcome = {"returncode": None, "timeout": True}
        append_jsonl(self.retry_path, {"case_id": case["case_id"], "original": kind, "attempt": 2, "seconds": round(time.time() - t0, 2),
                                       "counted_outcome_unchanged": True, **outcome})
        tmp.unlink(missing_ok=True)

    # ------------------------------------------------------------------ one shard
    def run_shard(self, n: int) -> None:
        plan_path, res, tr, prog, stem = self.shard_paths(n)
        plan = list(read_jsonl(plan_path))
        by_id = {c["case_id"]: c for c in plan}
        timeout_base = self.cfg["case_timeout_s"]
        while not self.stop.is_set():
            ensure_newline(res)
            ensure_newline(tr)
            done, statuses = set(), {}
            for r in read_jsonl(res):
                cid = r.get("case_id")
                statuses.setdefault(cid, []).append(r.get("status"))
                if r.get("status") == "COMPLETED":
                    done.add(cid)
            for cid, sts in statuses.items():
                if "INVALID_GENERATION" in sts:
                    self.halt(f"invalid test generation for {cid} (see {res})")
                    return
            for cid, sts in statuses.items():
                if cid not in done and sts.count("HARNESS_ERROR") > self.harness_fault_count.get(cid, 0):
                    for _ in range(sts.count("HARNESS_ERROR") - self.harness_fault_count.get(cid, 0)):
                        self.harness_fault(cid, "HARNESS_ERROR reported by worker")
            if self.stop.is_set():
                return
            remaining = [c for c in plan if c["case_id"] not in done]
            if not remaining:
                self.seal(n, plan, res, tr, stem)
                return
            if not self.budget_ok():
                return
            tmp_plan = self.prog_dir / f"{stem}.remaining.jsonl"
            tmp_plan.write_text("".join(json.dumps(c, separators=(",", ":")) + "\n" for c in remaining))
            img_cfg = self.cfg["images"]
            cmd = [str(self.worker), "--plan", str(tmp_plan), "--results", str(res), "--traces", str(tr), "--progress", str(prog),
                   "--images", str(self.img_dir), "--trace-uri-prefix", f"traces/{self.set}/{stem}.jsonl.gz",
                   "--fail-image-cap", str(img_cfg["fail_cap_per_bin"]), "--wrong-image-cap", str(img_cfg["wrong_payload_cap"]),
                   "--success-sample-ppm", str(img_cfg["success_sample_ppm"])]
            proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, start_new_session=True)
            last_size, last_change = -1, time.time()
            killed_for = None
            while True:
                rc = proc.poll()
                if rc is not None:
                    break
                if self.stop.is_set():
                    os.killpg(proc.pid, signal.SIGTERM)
                    proc.wait()
                    return
                size = prog.stat().st_size if prog.exists() else 0
                if size != last_size:
                    last_size, last_change = size, time.time()
                cid, _ = self.last_open_markers(prog)
                frames = int(by_id.get(cid, {}).get("frames", 1)) if cid else 1
                if time.time() - last_change > timeout_base * max(1, frames):
                    killed_for = cid
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
                    break
                time.sleep(0.5)
            err = proc.stderr.read().decode(errors="replace")[-2000:] if proc.stderr else ""
            new = self.count_new(res, done)
            if rc == 0 and killed_for is None:
                if new == 0:
                    self.harness_fault(f"{stem}:no-progress", f"worker exited 0 without completing any case: {err[-300:]}")
                continue  # loop re-reads results; seals when complete
            cid, phase = self.last_open_markers(prog)
            cid = cid or killed_for
            if cid is None or cid not in by_id:
                self.infra(kind="worker_exit_unattributed", shard=n, returncode=rc, stderr=err)
                self.harness_fault(f"{stem}:unattributed", f"worker exit {rc} without an open case")
                continue
            prog.open("a").write(f"END {cid}\n")  # close the bracket so the next pass starts clean
            kind = "TIMEOUT" if killed_for else "CRASH"
            if phase == "scan":
                result = self.reader_fault_result(by_id[cid], kind, f"returncode={rc} stderr_tail={err[-300:]!r}")
                append_jsonl(res, result)
                with self.totals.lock:
                    self.totals.reader_faults += 1
                self.infra(kind=f"reader_{kind.lower()}", case_id=cid, returncode=rc)
                self.log(f"  reader {kind.lower()} on {cid} (recorded as a completed failure; reproducing once)")
                self.reproduce(by_id[cid], kind)
            else:
                self.harness_fault(cid, f"worker {kind.lower()} outside the reader call (rc={rc}): {err[-300:]}")

    def count_new(self, res: Path, before: set) -> int:
        n = 0
        for r in read_jsonl(res):
            if r.get("status") == "COMPLETED" and r.get("case_id") not in before:
                n += 1
        return n

    def seal(self, n: int, plan: list, res: Path, tr: Path, stem: str) -> None:
        first: dict[str, dict] = {}
        dups = 0
        for r in read_jsonl(res):
            if r.get("status") != "COMPLETED":
                continue
            if r["case_id"] in first:
                dups += 1
                continue
            first[r["case_id"]] = r
        ids = [c["case_id"] for c in plan]
        missing = [i for i in ids if i not in first]
        if missing:
            raise Halt(f"seal {stem}: {len(missing)} cases without a result")
        stats = {"success": sum(1 for i in ids if first[i].get("success")), "wrong_payload": sum(1 for i in ids if first[i].get("wrong_payload")),
                 "reader_faults": sum(1 for i in ids if str(first[i].get("failure_reason", "")).startswith("READER_"))}
        gz_res, gz_tr = res.with_suffix(".jsonl.gz"), tr.with_suffix(".jsonl.gz")
        for src, dst in ((res, gz_res), (tr, gz_tr)):
            if src.exists():
                with open(src, "rb") as fi, gzip.GzipFile(dst, "wb", mtime=0) as fo:
                    shutil.copyfileobj(fi, fo)
                src.unlink()
                os.chmod(dst, 0o444)
        record = {"shard": n, "stem": stem, "cases": len(ids), "duplicate_lines": dups, **stats,
                  "results_sha256": file_sha256(gz_res), "traces_sha256": file_sha256(gz_tr) if gz_tr.exists() else None,
                  "plan_sha256": file_sha256(self.plan_dir / f"{stem}.jsonl.gz"), "worker_sha256": self.worker_sha,
                  "host": socket.gethostname(), "sealed_at": time.time()}
        append_jsonl(self.manifest_path, record)
        for leftover in self.prog_dir.glob(f"{stem}.remaining.jsonl"):
            leftover.unlink()
        self.offsite(stem)
        with self.totals.lock:
            self.totals.done += len(ids)
            self.totals.success += stats["success"]
            self.totals.wrong += stats["wrong_payload"]
        if stats["wrong_payload"]:
            self.log(f"  !!! {stats['wrong_payload']} WRONG PAYLOAD(S) in {stem} -- certification blocked; continuing the campaign")

    def offsite(self, stem: str) -> None:
        """Upload a sealed shard to R2 (if configured). Failures never affect results: `./ckt sync` retries."""
        if not self.cfg.get("storage", {}).get("r2", {}).get("upload_on_seal", True):
            return
        try:
            from storage.r2 import R2
            r2 = R2(self.data)
            if r2.enabled:
                r2.upload_shard(self.set, stem)
        except Exception as e:
            self.infra(kind="r2_upload_deferred", stem=stem, error=repr(e)[:300])

    # ------------------------------------------------------------------ run
    def run(self) -> dict:
        manifest = self.plan_manifest()
        total = manifest["cases"]
        sealed = self.sealed()
        todo = [s["shard"] for s in manifest["shards"] if s["shard"] not in sealed]
        already = sum(r["cases"] for r in sealed.values())
        self.log(f"[{self.set}] {total:,} cases in {len(manifest['shards'])} shards; {already:,} already sealed; {len(todo)} shards to run on {self.workers} workers")
        queue = list(todo)
        qlock = threading.Lock()

        def slot():
            while not self.stop.is_set():
                with qlock:
                    if not queue:
                        return
                    n = queue.pop(0)
                try:
                    self.run_shard(n)
                except Halt as e:
                    self.halt(str(e))
                except Exception as e:  # never swallow: an unexpected supervisor error halts
                    self.halt(f"supervisor error on shard {n}: {e!r}")

        prev = signal.getsignal(signal.SIGINT)
        signal.signal(signal.SIGINT, lambda *_: (self.log("\ninterrupt: stopping workers; resume later"), self.stop.set()))
        threads = [threading.Thread(target=slot, daemon=True) for _ in range(self.workers)]
        for t in threads:
            t.start()
        last = 0.0
        while any(t.is_alive() for t in threads):
            time.sleep(1)
            if time.time() - last > 30:
                last = time.time()
                self.status_line(total, already)
        signal.signal(signal.SIGINT, prev)
        self.status_line(total, already)
        sealed = self.sealed()
        complete = sum(r["cases"] for r in sealed.values())
        out = {"plan_set": self.set, "cases": total, "completed": complete, "complete": complete == total,
               "halted": self.halt_reason, "wrong_payloads": sum(r.get("wrong_payload", 0) for r in sealed.values())}
        if self.halt_reason:
            raise Halt(self.halt_reason)
        return out

    def inflight_done(self) -> int:
        """Cases finished in shards not yet sealed (END markers), for a live ETA."""
        n = 0
        for prog in self.prog_dir.glob("shard-*.log"):
            if (self.res_dir / (prog.stem + ".jsonl.gz")).exists():
                continue
            try:
                with open(prog, "r", errors="replace") as fh:
                    n += sum(1 for line in fh if line.startswith("END "))
            except OSError:
                pass
        return n

    def status_line(self, total: int, already: int) -> None:
        t = self.totals
        el = max(1.0, time.time() - t.started)
        live = t.done + self.inflight_done()
        rate = live / el
        left = total - already - live
        eta = left / rate / 3600 if rate > 0 else float("inf")
        self.log(f"[{self.set}] {already + live:,}/{total:,} done ({already + t.done:,} sealed)  {rate * 60:,.0f} cases/min  ETA {eta:,.1f} h  "
                 f"success(this run) {t.success:,}  WRONG {t.wrong}  reader faults {t.reader_faults}  harness faults {t.harness_faults}")
