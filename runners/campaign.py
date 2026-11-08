"""Campaign state machine and stage implementations (SPEC §8).

BASELINE_LOCKED -> GENERATE -> VALIDATE_ARTIFACT -> EXECUTE -> CLASSIFY -> REPORT_1M
-> SELECT_WEAKNESSES -> EXECUTE_500K -> PROPOSE_FIX -> BUILD -> REGRESSION
-> INDEPENDENT_HOLDOUT -> CERTIFICATION_REVIEW -> ARCHIVE

Every transition is appended to campaign/state/transitions.jsonl with the
hashes it relied on; nothing in this module deletes or rewrites results.
Stages are idempotent and resumable: re-running one continues where it
stopped. PROPOSE_FIX onward involves people (or separately-run agents):
this code builds, measures and reports, but never approves its own changes.
"""
from __future__ import annotations

import getpass
import hashlib
import json
import os
import platform
import secrets
import socket
import subprocess
import sys
import time
from pathlib import Path

from analysis.adaptive import select as adaptive
from analysis.metrics import db as dbm
from analysis.metrics import report as rpt
from certification.qualification import gates as gatesmod
from generator import plan as planmod
from generator.common import ROOT, capacity_table, file_sha256, load_yaml, scaled, test_scale, worker_path, worker_schema
from runners.sharding.supervisor import Halt, Supervisor, append_jsonl, read_jsonl
from security.artifact_signing import sign as signing

STATES = ["NEW", "BASELINE_LOCKED", "GENERATE", "VALIDATE_ARTIFACT", "EXECUTE", "CLASSIFY", "REPORT_1M", "SELECT_WEAKNESSES",
          "EXECUTE_500K", "PROPOSE_FIX", "BUILD", "REGRESSION", "INDEPENDENT_HOLDOUT", "CERTIFICATION_REVIEW", "ARCHIVE"]
LOCKED_CONFIGS = ["strata.yaml", "gates.yaml", "adaptive.yaml", "devices.yaml", "baseline.yaml"]


class Campaign:
    def __init__(self, data_dir: Path | None = None, log=print):
        cfg = load_yaml("campaign.yaml")
        self.data = Path(data_dir or (ROOT / cfg["data_dir"])).resolve()
        self.state_dir = self.data / "state"
        self.state_dir.mkdir(parents=True, exist_ok=True)
        self.log = log
        self.cfg = cfg

    # ------------------------------------------------------------------ state
    @property
    def state(self) -> str:
        p = self.state_dir / "state.json"
        return json.loads(p.read_text())["state"] if p.exists() else "NEW"

    def lock(self) -> dict | None:
        p = self.state_dir / "lock.json"
        return json.loads(p.read_text()) if p.exists() else None

    def transition(self, to: str, evidence: dict | None = None, note: str = "") -> None:
        frm = self.state
        if STATES.index(to) < STATES.index(frm):
            raise SystemExit(f"illegal transition {frm} -> {to} (states only move forward; record a new campaign for a re-run)")
        rec = {"from": frm, "to": to, "time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "user": getpass.getuser(),
               "host": socket.gethostname(), "note": note, "evidence": evidence or {}}
        append_jsonl(self.state_dir / "transitions.jsonl", rec)
        (self.state_dir / "state.json").write_text(json.dumps({"state": to, "since": rec["time"]}, indent=1))
        self.log(f"state: {frm} -> {to}")

    def require(self, *allowed: str) -> None:
        if self.state not in allowed:
            raise SystemExit(f"this step needs state {' or '.join(allowed)}; current state is {self.state}. Run ./ckt status")

    def config_hashes(self) -> dict:
        return {name: file_sha256(ROOT / "configs" / name) for name in LOCKED_CONFIGS}

    def check_lock_intact(self) -> None:
        lk = self.lock()
        if not lk:
            raise SystemExit("not locked")
        now = self.config_hashes()
        changed = [n for n in LOCKED_CONFIGS if now[n] != lk["configs"][n]]
        if changed:
            raise SystemExit(f"locked configuration changed since BASELINE_LOCKED: {changed}. Results would not be comparable; restore them (git checkout) or start a new campaign directory.")
        if file_sha256(worker_path()) != lk["worker_sha256"]:
            raise SystemExit("ckt_worker binary differs from the locked build; rebuild from the locked commit or start a new campaign")

    # ------------------------------------------------------------------ BASELINE_LOCKED
    def do_lock(self, approver: str) -> None:
        self.require("NEW")
        if not approver.strip():
            raise SystemExit("--approver is required: locking records your approval of configs/gates.yaml thresholds")
        base = load_yaml("baseline.yaml")
        ckey = ROOT / base["ckey"]["path"]
        head = subprocess.run(["git", "-C", str(ckey), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(ckey), "status", "--porcelain"], capture_output=True, text=True).stdout.strip()
        if head != base["ckey"]["commit"]:
            raise SystemExit(f"ckey checkout at {ckey} is {head or 'missing'}, expected {base['ckey']['commit']} (run scripts/preflight.sh)")
        if dirty:
            raise SystemExit("ckey checkout has local modifications; the baseline must be the pristine commit")
        harness = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        hdirty = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--", "configs", "simulation", "runners", "generator", "analysis"],
                                capture_output=True, text=True).stdout.strip()
        if hdirty:
            raise SystemExit("harness has uncommitted changes in locked paths; commit them so the lock names an immutable harness commit")
        st = subprocess.run([str(worker_path()), "--selftest"], capture_output=True, text=True)
        if st.returncode != 0:
            raise SystemExit("worker selftest failed; see `./build/native/ckt_worker --selftest`")
        cfg = json.loads(subprocess.run([str(worker_path()), "--print-config"], capture_output=True, text=True, check=True).stdout)
        if cfg["ckey_sha"] != base["ckey"]["commit"]:
            raise SystemExit(f"worker was built against ckey {cfg['ckey_sha']}, not the baseline commit")
        (self.state_dir / "capacity_table.json").write_text(json.dumps(capacity_table(), indent=1))
        secret = secrets.token_hex(32)
        sp = self.state_dir / "holdout_secret"
        sp.write_text(secret)
        os.chmod(sp, 0o600)
        key = signing.ensure_key(self.data / "keys")
        spec_hashes = {f: file_sha256(ckey / f) for f in base["ckey"]["spec_files"] + base["ckey"]["reader_reference"]}
        if test_scale() != 1:
            self.log(f"*** TEST SCALE 1/{test_scale()}: this campaign exercises the pipeline only and can never qualify ***")
        lock = {"spec_id": base["spec_id"], "locked_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "approver": approver,
                "approval_statement": f"{approver} approves the release gates in configs/gates.yaml (sha256 {file_sha256(ROOT / 'configs' / 'gates.yaml')}) and the pre-registered design before execution.",
                "ckey_commit": head, "harness_commit": harness, "configs": self.config_hashes(), "spec_files": spec_hashes,
                "worker_sha256": file_sha256(worker_path()), "worker_config": cfg, "case_schema_sha256": hashlib.sha256(json.dumps(worker_schema(), sort_keys=True).encode()).hexdigest(),
                "toolchain": {"python": sys.version, "platform": platform.platform(), "machine": platform.machine(), "cpus": os.cpu_count()},
                "holdout_seed_commitment": hashlib.sha256(secret.encode()).hexdigest(),
                "signing": "ssh-keygen ed25519" if key else "UNSIGNED (ssh-keygen unavailable)",
                "test_scale": test_scale(),
                "qualifying": test_scale() == 1}
        lp = self.state_dir / "lock.json"
        lp.write_text(json.dumps(lock, indent=1, sort_keys=True))
        os.chmod(lp, 0o444)
        sig = signing.sign(lp, self.data / "keys")
        self.transition("BASELINE_LOCKED", {"lock_sha256": file_sha256(lp), "signature": sig}, note=f"gates approved by {approver}")

    # ------------------------------------------------------------------ GENERATE
    def do_generate(self) -> None:
        self.require("BASELINE_LOCKED", "GENERATE")
        self.check_lock_intact()
        if self.state == "BASELINE_LOCKED":
            self.transition("GENERATE")
        scfg = load_yaml("strata.yaml")
        made = {}
        for name, master, div in (("pilot", "CKTEST-RH-001/pilot/v1", 1), ("structured", scfg["master_seed"], 1),
                                  ("holdout", f"CKTEST-RH-001/holdout/{(self.state_dir / 'holdout_secret').read_text().strip()}", 10)):
            if (self.data / "plans" / name / "manifest.json").exists():
                self.log(f"plan {name}: exists")
                continue
            self.log(f"generating plan {name} ...")
            m = planmod.build(name, self.data, master, scale_div=div)
            made[name] = {"cases": m["cases"], "plan_root_sha256": m["plan_root_sha256"]}
            self.log(f"  {name}: {m['cases']:,} cases, root {m['plan_root_sha256'][:16]}")
        self.log("plans generated")

    # ------------------------------------------------------------------ VALIDATE_ARTIFACT (incl. pilot)
    def do_validate(self) -> None:
        self.require("GENERATE", "VALIDATE_ARTIFACT")
        self.check_lock_intact()
        reports = {}
        for name, n in (("pilot", planmod.expected_total("pilot")), ("structured", planmod.expected_total("structured")), ("holdout", planmod.expected_total("holdout", 10))):
            self.log(f"validating plan {name} ...")
            rep = planmod.validate(self.data, name, n)
            reports[name] = {k: rep[k] for k in ("total", "ok", "problems", "generation_notes", "cell_sizes")}
            if not rep["ok"]:
                raise Halt(f"plan {name} failed validation: {rep['problems'][:5]}")
        (self.data / "reports").mkdir(parents=True, exist_ok=True)
        (self.data / "reports" / "plan_validation.json").write_text(json.dumps(reports, indent=1))
        if self.state == "GENERATE":
            self.transition("VALIDATE_ARTIFACT", {"plans": {k: v["total"] for k, v in reports.items()}})
        self.log("running the Phase 2 pilot (separate cohort, never counted) ...")
        Supervisor(self.data, "pilot", log=self.log).run()
        con = dbm.connect(self.data / "db" / "results.sqlite")
        dbm.ingest(con, self.data, "pilot")
        rep = rpt.cohort_report(con, "pilot")
        rpt.write(rep, self.data / "reports", "pilot", "Pilot (Phase 2, 10,000 cases; not counted)")
        infra = list(read_jsonl(self.data / "infra" / "pilot.jsonl"))
        pc = self.cfg["pilot"]
        cc_max = rep["oracle"]["renderer_crosscheck"]["max"] or 0
        problems = []
        if len([e for e in infra if e.get("kind") == "harness_fault"]) > pc["max_infra_failure_rate"] * planmod.expected_total("pilot"):
            problems.append("harness fault rate above threshold")
        if cc_max > pc["max_crosscheck_disagreement"]:
            problems.append(f"renderer cross-check disagreement {cc_max:.4f} above {pc['max_crosscheck_disagreement']}")
        if rep["oracle"]["independent_rs"]["mismatch"]:
            problems.append("independent RS disagreed with production parity")
        if rep["completed_cases"] != planmod.expected_total("pilot"):
            problems.append(f"pilot completed {rep['completed_cases']} of {planmod.expected_total('pilot')}")
        sims = [r[0] for r in con.execute("SELECT sim_ms + COALESCE(elapsed_ms,0) FROM cases WHERE cohort='pilot'")]
        mean_s = sum(sims) / max(1, len(sims)) / 1000
        workers = os.cpu_count()
        eta = {"mean_case_seconds_single_core": mean_s, "workers": workers,
               "structured_hours": 1e6 * mean_s / workers / 3600, "adaptive_hours": 5e5 * mean_s / workers / 3600, "holdout_hours": 1e5 * mean_s / workers / 3600}
        (self.data / "reports" / "pilot_gate.json").write_text(json.dumps({"problems": problems, "eta": eta}, indent=1))
        self.log(f"pilot: {rep['completed_cases']:,} cases, wrong payloads {rep['safety']['wrong_payloads_total']}, mean {mean_s:.2f} s/case/core -> "
                 f"structured ETA {eta['structured_hours']:.1f} h on {workers} workers")
        if problems:
            raise Halt("pilot validation failed: " + "; ".join(problems))

    # ------------------------------------------------------------------ EXECUTE
    def do_execute(self) -> None:
        self.require("VALIDATE_ARTIFACT", "EXECUTE")
        self.check_lock_intact()
        if not (self.data / "reports" / "pilot_gate.json").exists() or json.loads((self.data / "reports" / "pilot_gate.json").read_text())["problems"]:
            raise SystemExit("the pilot has not passed; run ./ckt validate")
        if self.state == "VALIDATE_ARTIFACT":
            self.transition("EXECUTE")
        out = Supervisor(self.data, "structured", log=self.log).run()
        if not out["complete"]:
            raise SystemExit("structured campaign stopped before completion; run ./ckt resume")

    # ------------------------------------------------------------------ CLASSIFY + REPORT_1M (freeze)
    def do_classify(self) -> None:
        self.require("EXECUTE", "CLASSIFY")
        con = dbm.connect(self.data / "db" / "results.sqlite")
        added = dbm.ingest(con, self.data, "structured")
        n = con.execute("SELECT COUNT(*) FROM cases WHERE cohort='structured'").fetchone()[0]
        self.log(f"ingested {added:,} rows; structured completed = {n:,}")
        want = planmod.expected_total("structured")
        if n != want:
            raise Halt(f"quota reconciliation: structured completed {n:,} != {want:,}")
        if self.state == "EXECUTE":
            self.transition("CLASSIFY", {"structured_completed": n})

    def roll_up(self, plan_sets: list[str]) -> dict:
        roots = {}
        for ps in plan_sets:
            recs = sorted(read_jsonl(self.data / "manifests" / f"{ps}.jsonl"), key=lambda r: r["shard"])
            roots[ps] = {"shards": len(recs), "cases": sum(r["cases"] for r in recs),
                         "root_sha256": hashlib.sha256("".join(r["results_sha256"] for r in recs).encode()).hexdigest()}
        return roots

    def do_report_1m(self) -> None:
        self.require("CLASSIFY", "REPORT_1M")
        con = dbm.connect(self.data / "db" / "results.sqlite")
        rep = rpt.cohort_report(con, "structured")
        j, m = rpt.write(rep, self.data / "reports", "structured_1m", "Structured campaign: 1,000,000 cases (frozen)")
        freeze = {"frozen_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "results": self.roll_up(["structured"]),
                  "report_json_sha256": file_sha256(j), "report_md_sha256": file_sha256(m),
                  "adaptive_rules_sha256": file_sha256(ROOT / "configs" / "adaptive.yaml"),
                  "statement": "Structured results frozen and published before any adaptive case was selected."}
        fp = self.data / "reports" / "structured_1m.freeze.json"
        fp.write_text(json.dumps(freeze, indent=1, sort_keys=True))
        os.chmod(fp, 0o444)
        sig = signing.sign(fp, self.data / "keys")
        for f in (self.data / "results" / "structured").glob("*.gz"):
            os.chmod(f, 0o444)
        from analysis.triage import triage as tri
        (self.data / "reports" / "counterexamples_structured.json").write_text(json.dumps(tri.catalogue(con, "structured"), indent=1))
        if self.state == "CLASSIFY":
            self.transition("REPORT_1M", {"freeze_sha256": file_sha256(fp), "signature": sig})
        self.log(f"report: {m}")

    # ------------------------------------------------------------------ SELECT + EXECUTE_500K
    def do_select(self) -> None:
        self.require("REPORT_1M", "SELECT_WEAKNESSES")
        if not (self.data / "reports" / "structured_1m.freeze.json").exists():
            raise SystemExit("the structured results are not frozen")
        freeze = json.loads((self.data / "reports" / "structured_1m.freeze.json").read_text())
        if freeze["adaptive_rules_sha256"] != file_sha256(ROOT / "configs" / "adaptive.yaml"):
            raise SystemExit("configs/adaptive.yaml changed after the freeze; adaptive rules must be the published ones")
        con = dbm.connect(self.data / "db" / "results.sqlite")
        scores = adaptive.weakness_scores(con)
        sel = self.data / "reports" / "adaptive_selection.json"
        if not sel.exists():
            sel.write_text(json.dumps({"weights": adaptive.ACFG["failure_targeted"]["weights"], "scores": scores}, indent=1))
        for name, fn in (("adaptive_ft", adaptive.failure_targeted), ("adaptive_cc", adaptive.cross_condition)):
            if not (self.data / "plans" / name / "manifest.json").exists():
                cases = fn(scores)
                planmod.write_cases(self.data, name, cases, {"pool": name})
                self.log(f"plan {name}: {len(cases):,} cases")
        b = adaptive.Boundary(self.state_dir / "boundary.json")
        if b.state is None:
            b.start(con, scores)
            self.log(f"boundary search: {len(b.state['tracks']):,} tracks")
        if self.state == "REPORT_1M":
            self.transition("SELECT_WEAKNESSES", {"selection_sha256": file_sha256(sel)})

    def do_execute_adaptive(self) -> None:
        self.require("SELECT_WEAKNESSES", "EXECUTE_500K")
        self.check_lock_intact()
        if self.state == "SELECT_WEAKNESSES":
            self.transition("EXECUTE_500K")
        con = dbm.connect(self.data / "db" / "results.sqlite")
        for name in ("adaptive_ft", "adaptive_cc"):
            Supervisor(self.data, name, log=self.log).run()
            dbm.ingest(con, self.data, name)
        b = adaptive.Boundary(self.state_dir / "boundary.json")
        while b.state["phase"] != "done":
            nxt = b.next_round()
            if nxt is None:
                b.state["phase"] = "done"
                b.save()
                break
            rname, cases = nxt
            ps = f"adaptive_bs_{rname}"
            if not (self.data / "plans" / ps / "manifest.json").exists():
                planmod.write_cases(self.data, ps, cases, {"pool": "boundary_search", "round": rname})
            self.log(f"boundary round {rname}: {len(cases):,} cases")
            Supervisor(self.data, ps, log=self.log).run()
            dbm.ingest(con, self.data, ps)
            results = {r[0]: {"success": r[1]} for r in con.execute("SELECT case_id, success FROM cases WHERE plan_set=?", (ps,))}
            if rname not in [x["round"] for x in b.state["rounds_done"]]:
                b.update(rname, results)
        n = con.execute("SELECT COUNT(*) FROM cases WHERE cohort='adaptive'").fetchone()[0]
        pools = dict(con.execute("SELECT pool, COUNT(*) FROM cases WHERE cohort='adaptive' GROUP BY pool").fetchall())
        rep = rpt.cohort_report(con, "adaptive")
        rep["pools"] = pools
        rep["boundary_counterexamples"] = b.counterexamples()
        rpt.write(rep, self.data / "reports", "adaptive_500k", "Adaptive campaign (conditional on selected weaknesses; NOT a field-prevalence estimate)")
        (self.data / "reports" / "minimal_counterexamples.json").write_text(json.dumps(b.counterexamples(1000), indent=1))
        self.log(f"adaptive completed: {n:,} cases {pools}")
        if n < scaled(500_000):
            raise Halt(f"adaptive completed {n:,} < {scaled(500_000):,}")
        freeze = {"frozen_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                  "results": self.roll_up([p.name[:-6] for p in (self.data / "manifests").glob("adaptive_*.jsonl")]), "pools": pools}
        fp = self.data / "reports" / "adaptive_500k.freeze.json"
        fp.write_text(json.dumps(freeze, indent=1, sort_keys=True))
        sig = signing.sign(fp, self.data / "keys")
        self.transition("PROPOSE_FIX", {"adaptive_completed": n, "freeze_sha256": file_sha256(fp), "signature": sig},
                        note="campaign evidence complete; fixes are proposed via reviewed PRs to clearkeysolutions/ckey")

    # ------------------------------------------------------------------ candidates, regression, holdout
    def build_candidate(self, ckey_path: Path, label: str) -> Path:
        self.require("PROPOSE_FIX", "BUILD", "REGRESSION")
        out = ROOT / "builds" / label
        subprocess.run(["cmake", "-S", str(ROOT / "runners" / "native"), "-B", str(out), "-DCMAKE_BUILD_TYPE=Release", f"-DCKEY_SOURCE_DIR={ckey_path}"], check=True)
        subprocess.run(["cmake", "--build", str(out), "-j", str(os.cpu_count())], check=True)
        st = subprocess.run([str(out / "ckt_worker"), "--selftest"])
        if st.returncode:
            raise SystemExit("candidate worker selftest failed")
        if self.state == "PROPOSE_FIX":
            self.transition("BUILD", {"candidate": label, "worker_sha256": file_sha256(out / "ckt_worker")})
        return out / "ckt_worker"

    def golden_cases(self) -> list[dict]:
        per = self.cfg["golden"]["per_stratum"]
        counts: dict = {}
        out = []
        for c in planmod.iter_plan(self.data, "structured"):
            if counts.get(c["stratum"], 0) < per:
                counts[c["stratum"]] = counts.get(c["stratum"], 0) + 1
                out.append(c)
        return out

    def regress(self, label: str, worker: Path) -> dict:
        self.require("BUILD", "REGRESSION")
        ps = f"regression_{label}"
        if not (self.data / "plans" / ps / "manifest.json").exists():
            planmod.write_cases(self.data, ps, self.golden_cases(), {"candidate": label})
        Supervisor(self.data, ps, worker=worker, log=self.log).run()
        con = dbm.connect(self.data / "db" / "results.sqlite")
        dbm.ingest(con, self.data, ps)
        # Compare on identical case ids against the frozen baseline results.
        import sqlite3
        new = sqlite3.connect(":memory:")
        new.execute("CREATE TABLE cases (case_id TEXT, stratum TEXT, profile TEXT, sev_bin INTEGER, success INTEGER)")
        new.executemany("INSERT INTO cases VALUES (?,?,?,?,?)", con.execute("SELECT case_id, stratum, profile, sev_bin, success FROM cases WHERE plan_set=?", (ps,)))
        base = sqlite3.connect(":memory:")
        base.execute("CREATE TABLE cases (case_id TEXT, success INTEGER)")
        base.executemany("INSERT INTO cases VALUES (?,?)", con.execute("SELECT case_id, success FROM cases WHERE plan_set='structured'"))
        res = gatesmod.regression(new, base, load_yaml("gates.yaml")["gates"]["G5_no_regression"])
        wrong = con.execute("SELECT COUNT(*) FROM cases WHERE plan_set=? AND wrong_payload=1", (ps,)).fetchone()[0]
        res["wrong_payloads"] = wrong
        (self.data / "reports" / f"{ps}.json").write_text(json.dumps(res, indent=1))
        if self.state == "BUILD":
            self.transition("REGRESSION", {"candidate": label, "result": res["status"], "wrong_payloads": wrong})
        return res

    def holdout(self, label: str, worker: Path) -> None:
        self.require("REGRESSION", "INDEPENDENT_HOLDOUT")
        if self.state == "REGRESSION":
            self.transition("INDEPENDENT_HOLDOUT", {"candidate": label})
        Supervisor(self.data, "holdout", worker=worker, log=self.log).run()
        con = dbm.connect(self.data / "db" / "results.sqlite")
        dbm.ingest(con, self.data, "holdout")
        rep = rpt.cohort_report(con, "holdout")
        rep["candidate"] = label
        rpt.write(rep, self.data / "reports", "holdout", f"Independent holdout (100,000 cases) on {label}")

    def review(self) -> dict:
        self.require("INDEPENDENT_HOLDOUT", "CERTIFICATION_REVIEW")
        con = dbm.connect(self.data / "db" / "results.sqlite")
        res = gatesmod.evaluate(con, self.data, (self.lock() or {}).get("configs", {}).get("gates.yaml"))
        (self.data / "reports" / "gates.json").write_text(json.dumps(res, indent=1, default=str))
        notes = list((ROOT / "agents" / "review_notes").glob("*.md")) if (ROOT / "agents" / "review_notes").exists() else []
        if self.state == "INDEPENDENT_HOLDOUT":
            self.transition("CERTIFICATION_REVIEW", {"label": res.get("label"), "review_notes": len(notes)})
        return res

    def status(self) -> dict:
        st = {"state": self.state, "data_dir": str(self.data), "lock": bool(self.lock())}
        for ps in sorted(p.name for p in (self.data / "plans").iterdir()) if (self.data / "plans").exists() else []:
            m = json.loads((self.data / "plans" / ps / "manifest.json").read_text())
            sealed = sum(r["cases"] for r in read_jsonl(self.data / "manifests" / f"{ps}.jsonl"))
            wrong = sum(r.get("wrong_payload", 0) for r in read_jsonl(self.data / "manifests" / f"{ps}.jsonl"))
            st[ps] = {"planned": m["cases"], "sealed": sealed, "wrong_payloads": wrong}
        return st
