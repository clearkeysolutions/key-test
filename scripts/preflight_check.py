"""Python half of preflight: consistency checks and a throughput benchmark."""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from generator.common import PROFILES, load_yaml, worker_path, worker_schema  # noqa: E402
from generator.strata import STRATA, make_case  # noqa: E402


def main() -> None:
    problems = []
    cfg = json.loads(subprocess.run([str(worker_path()), "--print-config"], capture_output=True, text=True, check=True).stdout)
    strata = load_yaml("strata.yaml")
    if cfg["recoverability"] != strata["recoverability"]["version"]:
        problems.append("recoverability version mismatch between worker and configs/strata.yaml")
    if cfg["ckey_sha"] != load_yaml("baseline.yaml")["ckey"]["commit"]:
        problems.append(f"worker built against {cfg['ckey_sha']}, not the baseline commit")
    schema = worker_schema()["fields"]
    sample = []
    for i, s in enumerate(sorted(STRATA)):
        for j, p in enumerate(PROFILES):
            for mode in ("sweep", "joint", "negative"):
                c = make_case("preflight", "CKTEST-RH-001/preflight", s, p, (i + j) % 5, mode, i * 7 + j)
                c.pop("_notes", None)
                bad = [k for k in c if k not in schema]
                if bad:
                    problems.append(f"generator emits fields unknown to the worker: {bad}")
                sample.append(c)
    print(f"schema: {len(schema)} worker fields; {len(sample)} generated cases conform" if not problems else "\n".join(problems))
    # Throughput: one representative slice per CPU, run in parallel like the campaign.
    n_workers = os.cpu_count() or 1
    tmp = Path(tempfile.mkdtemp(prefix="ckt-preflight-"))
    per = max(4, 96 // n_workers)
    import random
    random.Random(7).shuffle(sample)
    chunks = [sample[k * per:(k + 1) * per] for k in range(n_workers)]
    procs = []
    t0 = time.time()
    for k, ch in enumerate(chunks):
        if not ch:
            continue
        plan = tmp / f"p{k}.jsonl"
        plan.write_text("".join(json.dumps(c) + "\n" for c in ch))
        procs.append(subprocess.Popen([str(worker_path()), "--plan", str(plan), "--results", str(tmp / f"r{k}.jsonl")]))
    for p in procs:
        p.wait()
    wall = time.time() - t0
    rows = [json.loads(l) for k in range(n_workers) if (tmp / f"r{k}.jsonl").exists() for l in open(tmp / f"r{k}.jsonl")]
    bad = [r for r in rows if r.get("status") != "COMPLETED"]
    if bad:
        problems.append(f"{len(bad)} benchmark cases did not complete: {bad[:2]}")
    done = len(rows)
    per_case_wall = wall / max(1, done)
    eta = {"workers": n_workers, "benchmark_cases": done, "wall_s": round(wall, 1), "cases_per_hour": round(3600 / per_case_wall),
           "structured_1m_hours": round(1e6 * per_case_wall / 3600, 1), "adaptive_500k_hours": round(5e5 * per_case_wall / 3600, 1),
           "pilot_10k_hours": round(1e4 * per_case_wall / 3600, 2), "holdout_100k_hours": round(1e5 * per_case_wall / 3600, 1)}
    disk_gb = shutil.disk_usage(ROOT).free / 1e9
    est_gb = 1.6e6 * 2.2e3 / 1e9 + 8  # ~2.2 KB/case results+traces (gz) + images + DB headroom
    if disk_gb < est_gb + 15:
        problems.append(f"free disk {disk_gb:.0f} GB < estimated need {est_gb + 15:.0f} GB")
    shutil.rmtree(tmp, ignore_errors=True)
    out = {"time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "worker_config": cfg, "eta": eta, "free_disk_gb": round(disk_gb),
           "estimated_disk_gb": round(est_gb), "problems": problems, "ok": not problems}
    rep = ROOT / "campaign" / "reports"
    rep.mkdir(parents=True, exist_ok=True)
    (rep / "preflight.json").write_text(json.dumps(out, indent=1))
    print(json.dumps(eta, indent=1))
    print(f"free disk {disk_gb:.0f} GB (estimated need ~{est_gb:.0f} GB)")
    total = eta["pilot_10k_hours"] + eta["structured_1m_hours"] + eta["adaptive_500k_hours"]
    print(f"\nEstimated wall time for pilot + 1,000,000 + 500,000 on this machine: ~{total:.0f} h "
          f"(+ ~{eta['holdout_100k_hours']:.0f} h for the holdout, run later on a candidate).")
    if problems:
        print("PREFLIGHT PROBLEMS:\n  " + "\n  ".join(problems))
        sys.exit(1)
    print("PREFLIGHT OK")


if __name__ == "__main__":
    main()
