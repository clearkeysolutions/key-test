"""Plan generation: cohorts -> deterministic, shuffled, sharded case plans with manifests.

Cohorts
  pilot       10,000  Phase 2 validation; separate seed; never counted.
  structured  1,000,000  20 strata x 4 profiles x 5 bins x (250 negative + 1,100 sweep + 1,150 joint).
  holdout     100,000  same design at 1/10 scale, sealed master seed, distinct payload families,
              device fixed-pattern units and 50% of textures from ckey's own svg_raster.h (rendering-pipeline split).
  adaptive    built later by analysis/adaptive (pool field set per case).
Shards hold `shard_size` cases in a seeded random order, so every shard is a
representative mix and partial progress is never biased toward one stratum.
"""
from __future__ import annotations

import gzip
import hashlib
import json
import os
import random
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

from generator.common import PROFILES, derive_seed, load_yaml, scaled, test_scale, worker_schema
from generator.strata import STRATA, make_case

CFG = load_yaml("strata.yaml")


PILOT_CELL = {"negative": 5, "sweep": 22, "joint": 23}   # 50 per cell x 200 cells = 10,000


def cell_counts(scale_div: int = 1, cohort: str = "") -> dict:
    if test_scale() != 1:  # keeps the >=10% negative and >=40% sweep rules satisfiable at tiny scale
        return {"negative": 1, "sweep": 2, "joint": 2}
    return dict(PILOT_CELL) if cohort == "pilot" else {m: CFG["per_profile_bin"][m] // scale_div for m in ("negative", "sweep", "joint")}


def expected_total(cohort: str, scale_div: int = 1) -> int:
    return sum(cell_counts(scale_div, cohort).values()) * len(STRATA) * len(PROFILES) * CFG["severity_bins"]


def design_index(cohort: str, scale_div: int = 1) -> list[tuple]:
    counts = cell_counts(scale_div, cohort)
    idx = []
    for s in sorted(STRATA):
        for p in PROFILES:
            for b in range(CFG["severity_bins"]):
                for mode, n in counts.items():
                    idx += [(s, p, b, mode, k) for k in range(n)]
    return idx


def strip_private(case: dict) -> tuple[dict, list]:
    notes = case.pop("_notes", [])
    return case, notes


def _write_shard(args) -> dict:
    out_dir, shard_no, cohort, master, items, extra = args
    path = Path(out_dir) / f"shard-{shard_no:05d}.jsonl.gz"
    notes = Counter()
    lines = []
    for (s, p, b, mode, k) in items:
        case = make_case(cohort, master, s, p, b, mode, k, family_size=CFG["payload_family_size"])
        if extra:
            case.update(extra(case, k))
        case, n = strip_private(case)
        for x in n:
            notes[f"{s}/{x}"] += 1
        lines.append(json.dumps(case, separators=(",", ":"), sort_keys=True))
    data = ("\n".join(lines) + "\n").encode()
    with gzip.GzipFile(path, "wb", mtime=0) as fh:  # mtime=0: byte-identical regeneration
        fh.write(data)
    return {"shard": shard_no, "file": path.name, "cases": len(lines), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "plan_sha256": hashlib.sha256(data).hexdigest(), "notes": dict(notes)}


def holdout_extra(case: dict, k: int) -> dict:
    # Rendering-pipeline split: half the holdout textures come from ckey's svg_raster.h instead of the harness rasterizer.
    return {"rasterizer": "ckey" if derive_seed("holdout-raster", case["case_id"]) % 2 else "harness"}


def build(cohort: str, data_dir: Path, master: str, scale_div: int = 1, workers: int | None = None, order_seed: str | None = None) -> dict:
    out_dir = data_dir / "plans" / cohort
    if out_dir.exists() and any(out_dir.iterdir()):
        raise SystemExit(f"plan for {cohort} already exists at {out_dir}; plans are immutable (delete only before BASELINE_LOCKED)")
    out_dir.mkdir(parents=True, exist_ok=True)
    idx = design_index(cohort, scale_div)
    random.Random(derive_seed(order_seed or master, "shuffle", cohort)).shuffle(idx)
    size = CFG["shard_size"] if test_scale() == 1 else 100
    extra = holdout_extra if cohort == "holdout" else None
    jobs = [(str(out_dir), i // size, cohort, master, idx[i:i + size], extra) for i in range(0, len(idx), size)]
    import multiprocessing  # Python 3.14 defaults to forkserver on Linux; fork keeps imports simple
    with ProcessPoolExecutor(max_workers=workers or os.cpu_count(), mp_context=multiprocessing.get_context("fork")) as ex:
        shards = list(ex.map(_write_shard, jobs, chunksize=4))
    manifest = {"cohort": cohort, "cases": sum(s["cases"] for s in shards), "shards": shards,
                "design": {"scale_div": scale_div, "cell_counts": cell_counts(scale_div, cohort), "strata": len(STRATA), "profiles": list(PROFILES)},
                "plan_root_sha256": hashlib.sha256("".join(s["plan_sha256"] for s in shards).encode()).hexdigest()}
    if cohort != "holdout":
        manifest["master_seed"] = master
    else:
        manifest["master_seed_commitment"] = hashlib.sha256(master.encode()).hexdigest()
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True))
    return manifest


def iter_plan(data_dir: Path, cohort: str):
    pdir = data_dir / "plans" / cohort
    for f in sorted(pdir.glob("shard-*.jsonl.gz")):
        with gzip.open(f, "rt") as fh:
            for line in fh:
                if line.strip():
                    yield json.loads(line)


def validate(data_dir: Path, cohort: str, expect_total: int | None = None) -> dict:
    """Quota reconciliation and schema validation of a written plan (VALIDATE_ARTIFACT)."""
    schema = worker_schema()["fields"]
    pdir = data_dir / "plans" / cohort
    manifest = json.loads((pdir / "manifest.json").read_text())
    problems = []
    for s in manifest["shards"]:
        f = pdir / s["file"]
        if hashlib.sha256(f.read_bytes()).hexdigest() != s["sha256"]:
            problems.append(f"shard {s['file']} hash mismatch")
    ids = set()
    by_cell, by_stratum, by_mode, by_profile = Counter(), Counter(), Counter(), Counter()
    neg_by_stratum = Counter()
    total = 0
    for c in iter_plan(data_dir, cohort):
        total += 1
        bad = [k for k in c if k not in schema]
        if bad:
            problems.append(f"{c.get('case_id')}: unknown fields {bad}")
        if c["case_id"] in ids:
            problems.append(f"duplicate case_id {c['case_id']}")
        ids.add(c["case_id"])
        by_cell[(c["stratum"], c["profile"], c["sev_bin"])] += 1
        by_stratum[c["stratum"]] += 1
        by_profile[(c["stratum"], c["profile"])] += 1
        by_mode[(c["stratum"], c["mode"])] += 1
        if c["mode"] == "negative":
            neg_by_stratum[c["stratum"]] += 1
        if len(problems) > 50:
            break
    if expect_total is not None and total != expect_total:
        problems.append(f"total {total} != expected {expect_total}")
    for s, n in by_stratum.items():
        if by_mode[(s, "sweep")] < 0.4 * n:
            problems.append(f"{s}: single-factor sweeps {by_mode[(s, 'sweep')]} < 40% of {n}")
        if neg_by_stratum[s] < 0.1 * n:
            problems.append(f"{s}: negatives {neg_by_stratum[s]} < 10% of {n}")
    cells = sorted(set(by_cell.values()))
    notes = Counter()
    for s in manifest["shards"]:
        notes.update(s.get("notes", {}))
    report = {"cohort": cohort, "total": total, "strata": dict(sorted(by_stratum.items())), "cell_sizes": cells,
              "per_stratum_profile": {f"{s}/{p}": n for (s, p), n in sorted(by_profile.items())},
              "generation_notes": dict(notes), "problems": problems, "ok": not problems}
    return report


def write_cases(data_dir: Path, plan_set: str, cases: list[dict], meta: dict | None = None) -> dict:
    """Write an explicit list of cases (adaptive pools, boundary rounds, regression) as a plan set."""
    out_dir = data_dir / "plans" / plan_set
    if out_dir.exists() and any(out_dir.iterdir()):
        raise SystemExit(f"plan set {plan_set} already exists; plans are immutable")
    out_dir.mkdir(parents=True, exist_ok=True)
    size = CFG["shard_size"] if test_scale() == 1 else 100
    shards = []
    for i in range(0, len(cases), size):
        chunk = [strip_private(dict(c))[0] for c in cases[i:i + size]]
        data = ("\n".join(json.dumps(c, separators=(",", ":"), sort_keys=True) for c in chunk) + "\n").encode()
        path = out_dir / f"shard-{i // size:05d}.jsonl.gz"
        with gzip.GzipFile(path, "wb", mtime=0) as fh:
            fh.write(data)
        shards.append({"shard": i // size, "file": path.name, "cases": len(chunk), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                       "plan_sha256": hashlib.sha256(data).hexdigest(), "notes": {}})
    manifest = {"cohort": cases[0]["cohort"] if cases else None, "plan_set": plan_set, "cases": len(cases), "shards": shards, "meta": meta or {},
                "plan_root_sha256": hashlib.sha256("".join(s["plan_sha256"] for s in shards).encode()).hexdigest()}
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True))
    return manifest
