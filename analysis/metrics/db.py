"""Results database: SQLite built from sealed, hashed result shards.

The DB is a derived index, never the source of truth: it can be rebuilt at
any time from campaign/results/**.jsonl.gz (whose hashes are in the signed
manifests). Ingest keeps the FIRST completed result per (plan_set, case_id).
Plan fields that results do not carry (substrate, device geometry, pool,
ideal, frames, ...) are joined from the plan shard with the same number.
"""
from __future__ import annotations

import json
import sqlite3
from pathlib import Path

from runners.sharding.supervisor import read_jsonl

RESULT_COLS = [
    ("case_id", "TEXT"), ("cohort", "TEXT"), ("stratum", "TEXT"), ("profile", "TEXT"), ("mode", "TEXT"), ("sev_bin", "INTEGER"),
    ("factor", "TEXT"), ("severity", "REAL"), ("payload_family", "TEXT"), ("payload_hash", "TEXT"), ("payload_len", "INTEGER"),
    ("format_version", "TEXT"), ("seed", "TEXT"), ("renderer_sha", "TEXT"), ("scanner_sha", "TEXT"), ("model_sha", "TEXT"),
    ("harness_sha", "TEXT"), ("camera_model", "TEXT"), ("transformations", "TEXT"), ("image_hash", "TEXT"),
    ("expected_outcome", "TEXT"), ("observed_outcome", "TEXT"), ("success", "INTEGER"), ("wrong_payload", "INTEGER"),
    ("accepted_match", "TEXT"), ("accepted_profile", "INTEGER"), ("neg_kind", "TEXT"), ("finder_detected", "INTEGER"),
    ("orientation_correct", "INTEGER"), ("localization_error", "REAL"), ("localization_error_modules", "REAL"),
    ("FEC_corrections", "INTEGER"), ("erasures", "INTEGER"), ("frame_count", "INTEGER"), ("frames_planned", "INTEGER"),
    ("success_frame", "INTEGER"), ("elapsed_ms", "REAL"), ("decode_ms_first", "REAL"), ("tts_ms", "REAL"), ("sim_ms", "REAL"),
    ("peak_memory", "INTEGER"), ("failure_reason", "TEXT"), ("trace_uri", "TEXT"), ("image_uri", "TEXT"), ("borders", "INTEGER"),
    ("gt_rank", "INTEGER"), ("gt_quad_decodes", "INTEGER"), ("frame_w", "INTEGER"), ("frame_h", "INTEGER"), ("p0_version", "INTEGER"),
    ("read_version", "INTEGER"), ("phys_class", "TEXT"), ("phys_reason", "TEXT"), ("ppm_min", "REAL"), ("ppm_mean", "REAL"),
    ("blur_px", "REAL"), ("blur_modules", "REAL"), ("mtf", "REAL"), ("dprime", "REAL"), ("contrast", "REAL"), ("occluded", "REAL"),
    ("saturated", "REAL"), ("byte_err_est", "REAL"), ("in_frame", "INTEGER"), ("ecc_fraction", "REAL"), ("rs_verified", "INTEGER"),
    ("crosscheck", "REAL"), ("texture_px", "REAL"), ("electrons_white", "REAL"), ("digital_gain", "REAL"), ("versions", "TEXT"),
    ("supervisor_detail", "TEXT"),
]
PLAN_COLS = [("substrate", "TEXT"), ("ideal", "INTEGER"), ("pool", "TEXT"), ("rasterizer", "TEXT"), ("device", "TEXT"),
             ("symbol_mm", "REAL"), ("distance_mm", "REAL"), ("target_ppm", "REAL"), ("color_mode", "TEXT"), ("p0_layout", "TEXT"),
             ("p0_level", "TEXT"), ("p3_tier", "INTEGER"), ("payload_pattern", "TEXT"), ("occl_kind", "TEXT"), ("second_code", "INTEGER")]
PLAN_DEFAULTS = {"substrate": "ideal", "ideal": 0, "pool": "", "rasterizer": "harness", "color_mode": "mono", "occl_kind": "none", "second_code": 0}


def connect(path: Path) -> sqlite3.Connection:
    path.parent.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(path)
    con.execute("PRAGMA journal_mode=WAL")
    cols = ", ".join(f"{n} {t}" for n, t in RESULT_COLS + PLAN_COLS)
    con.execute(f"CREATE TABLE IF NOT EXISTS cases (plan_set TEXT, {cols}, PRIMARY KEY (plan_set, case_id))")
    con.execute("CREATE TABLE IF NOT EXISTS ingested (plan_set TEXT, shard INTEGER, results_sha256 TEXT, PRIMARY KEY (plan_set, shard))")
    for idx in ("cohort, stratum, profile, sev_bin", "failure_reason", "wrong_payload", "phys_class"):
        name = "i_" + "".join(ch if ch.isalnum() else "_" for ch in idx)
        con.execute(f"CREATE INDEX IF NOT EXISTS {name} ON cases ({idx})")
    return con


def _norm(v):
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, (list, dict)):
        return json.dumps(v)
    return v


def ingest(con: sqlite3.Connection, data_dir: Path, plan_set: str) -> int:
    """Ingest every sealed shard of a plan set not yet in the DB; returns rows added."""
    manifest = data_dir / "manifests" / f"{plan_set}.jsonl"
    seen = {r[0] for r in con.execute("SELECT shard FROM ingested WHERE plan_set=?", (plan_set,))}
    added = 0
    names = [n for n, _ in RESULT_COLS] + [n for n, _ in PLAN_COLS]
    sql = f"INSERT OR IGNORE INTO cases (plan_set, {', '.join(names)}) VALUES ({', '.join('?' * (len(names) + 1))})"
    for rec in read_jsonl(manifest):
        if rec["shard"] in seen:
            continue
        plan = {c["case_id"]: c for c in read_jsonl(data_dir / "plans" / plan_set / f"{rec['stem']}.jsonl.gz")}
        rows = []
        for r in read_jsonl(data_dir / "results" / plan_set / f"{rec['stem']}.jsonl.gz"):
            if r.get("status") != "COMPLETED":
                continue
            p = plan.get(r["case_id"], {})
            vals = [_norm(r.get(n)) for n, _ in RESULT_COLS] + [_norm(p.get(n, PLAN_DEFAULTS.get(n))) for n, _ in PLAN_COLS]
            rows.append([plan_set] + vals)
        before = con.total_changes
        con.executemany(sql, rows)
        added += con.total_changes - before
        con.execute("INSERT INTO ingested VALUES (?,?,?)", (plan_set, rec["shard"], rec["results_sha256"]))
        con.commit()
    return added
