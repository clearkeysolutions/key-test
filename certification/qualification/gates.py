"""Release-gate evaluation (SPEC §10) and label assignment.

The evaluator only runs against gates.yaml whose SHA-256 equals the one
recorded at BASELINE_LOCKED, so thresholds cannot move after results exist.
Each gate returns PASS, FAIL or NOT_EVALUATED with its evidence. The highest
label this code can assign is CERTIFICATION_CANDIDATE; CERTIFIED requires a
signed disposition from the independent certification reviewer and is never
produced automatically.
"""
from __future__ import annotations

import json
import math
import sqlite3
from pathlib import Path

from analysis.statistics.stats import one_sided_upper, quantile, wilson
from generator.common import ROOT, file_sha256, load_yaml

GATES_PATH = ROOT / "configs" / "gates.yaml"


def evaluate(con: sqlite3.Connection, data_dir: Path, locked_sha: str | None, baseline_db: sqlite3.Connection | None = None) -> dict:
    cur_sha = file_sha256(GATES_PATH)
    if locked_sha is None:
        return {"error": "gates were never locked (run ./ckt lock)"}
    if cur_sha != locked_sha:
        return {"error": f"configs/gates.yaml changed after the lock ({cur_sha[:12]} != {locked_sha[:12]}); refusing to evaluate"}
    G = load_yaml("gates.yaml")["gates"]
    strata = load_yaml("strata.yaml")
    out: dict = {}

    def count(sql, args=()):
        return con.execute(sql, args).fetchone()

    # G1: no wrong payloads in structured + adaptive + holdout.
    n, w = count("SELECT COUNT(*), COALESCE(SUM(wrong_payload),0) FROM cases WHERE cohort IN ('structured','adaptive','holdout')")
    hold = count("SELECT COUNT(*) FROM cases WHERE cohort='holdout'")[0]
    status = "FAIL" if w > G["G1_no_wrong_payload"]["max_wrong_payloads"] else ("PASS" if hold > 0 else "NOT_EVALUATED")
    out["G1_no_wrong_payload"] = {"status": status, "cases": n, "wrong_payloads": int(w), "upper95_rate": one_sided_upper(int(w), n) if n else None,
                                  "holdout_cases": hold, "note": "NOT_EVALUATED until the independent holdout has run" if hold == 0 else ""}
    # G2: clean exact round trip at or above the declared minimum resolution.
    g2 = {}
    ok_all = True
    any_cases = False
    for prof, dmin in strata["declared_min_ppm"].items():
        n2, k2 = count("""SELECT COUNT(*), COALESCE(SUM(success),0) FROM cases WHERE cohort='structured' AND stratum='S01' AND neg_kind='none'
                          AND expected_outcome='DECODE' AND variant=? AND ppm_min >= ? AND phys_class!='UNRECOVERABLE'""", (prof, dmin))
        g2[prof] = {"cases": n2, "success": int(k2), "failures": n2 - int(k2), "declared_min_ppm": dmin}
        any_cases |= n2 > 0
        ok_all &= n2 == int(k2)
    out["G2_clean_round_trip"] = {"status": ("PASS" if ok_all else "FAIL") if any_cases else "NOT_EVALUATED", "by_profile": g2}
    # G3: weighted normal-use success.
    cfg3 = G["G3_normal_use"]
    est, var, detail = 0.0, 0.0, {}
    wsum = 0.0
    for s, wgt in cfg3["use_weights"].items():
        n3, k3 = count("""SELECT COUNT(*), COALESCE(SUM(success),0) FROM cases WHERE cohort='structured' AND stratum=? AND neg_kind='none'
                          AND expected_outcome IN ('DECODE','DECODE_ANY_OF') AND phys_class='RECOVERABLE' AND sev_bin<=?""", (s, cfg3["selector"]["sev_bin_max"]))
        if n3 == 0:
            continue
        p = int(k3) / n3
        lo, hi = wilson(int(k3), n3)
        detail[s] = {"n": n3, "rate": p, "wilson95": [lo, hi], "weight": wgt}
        est += wgt * p
        var += wgt * wgt * max(p * (1 - p), 1 / (n3 * n3)) / n3
        wsum += wgt
    if wsum:
        est /= wsum
        se = math.sqrt(var) / wsum
        lower = est - 1.959964 * se
        out["G3_normal_use"] = {"status": "PASS" if est >= cfg3["min_success_rate"] else "FAIL", "weighted_success": est, "lower95": lower,
                                "lower_bound_meets_threshold": lower >= cfg3["min_success_rate"], "by_stratum": detail}
    else:
        out["G3_normal_use"] = {"status": "NOT_EVALUATED"}
    # G4: device latency (host timings cannot satisfy it).
    host = [r[0] for r in con.execute("SELECT decode_ms_first FROM cases WHERE cohort='structured' AND decode_ms_first IS NOT NULL AND frame_count=1")]
    dev = G["G4_latency"]["reference_device"]
    out["G4_latency"] = {"status": "NOT_EVALUATED", "reason": "requires measurements on the reference iOS device" + ("" if dev else " (not yet named by the owner)"),
                         "host_p95_ms_indicative": quantile(host, .95)}
    # G5: regression vs baseline (only for candidate builds).
    out["G5_no_regression"] = {"status": "NOT_EVALUATED", "reason": "evaluated per candidate build by ./ckt regress"} if baseline_db is None else regression(con, baseline_db, G["G5_no_regression"])
    # G6: memory safety and bounded compute.
    faults = count("SELECT COUNT(*) FROM cases WHERE failure_reason LIKE 'READER_%'")[0]
    timeouts = count("SELECT COUNT(*) FROM cases WHERE failure_reason='READER_TIMEOUT'")[0]
    peak = count("SELECT MAX(peak_memory) FROM cases")[0] or 0
    sec = data_dir / "reports" / "security.json"
    secrep = json.loads(sec.read_text()) if sec.exists() else None
    cfg6 = G["G6_memory_safety"]
    fail6 = faults > cfg6["max_crashes"] or timeouts > cfg6["max_timeouts"] or peak / 1048576 > cfg6["max_peak_memory_mb"]
    if secrep:
        fail6 |= secrep.get("sanitizer_findings", 1) > cfg6["max_sanitizer_findings"] or secrep.get("fuzz_crashes", 1) > 0
    out["G6_memory_safety"] = {"status": "FAIL" if fail6 else ("PASS" if secrep and secrep.get("fuzz_minutes", 0) >= cfg6["fuzz_minutes_min"] else "NOT_EVALUATED"),
                               "reader_crashes_or_timeouts": faults, "timeouts": timeouts, "peak_memory_mb": peak / 1048576, "security_report": secrep}
    # G7: coverage.
    cfg7 = G["G7_coverage"]
    ns = count("SELECT COUNT(*) FROM cases WHERE cohort='structured'")[0]
    na = count("SELECT COUNT(*) FROM cases WHERE cohort='adaptive'")[0]
    nh = hold
    synthetic_ok = ns == cfg7["structured_exact"] and na >= cfg7["adaptive_min"]
    out["G7_coverage"] = {"status": "FAIL" if not synthetic_ok else ("PASS" if nh >= cfg7["holdout_min"] and not cfg7["real_device_required"] else "NOT_EVALUATED"),
                          "structured": ns, "adaptive": na, "holdout": nh, "real_device": "not provided",
                          "synthetic_coverage_ok": synthetic_ok and nh >= cfg7["holdout_min"]}
    out["label"] = label(out)
    out["gates_sha256"] = cur_sha
    return out


def label(g: dict) -> str:
    def ok(name):
        return g.get(name, {}).get("status") == "PASS"
    synthetic = all(ok(x) for x in ("G1_no_wrong_payload", "G2_clean_round_trip", "G3_normal_use", "G6_memory_safety")) \
        and g.get("G7_coverage", {}).get("synthetic_coverage_ok") and g.get("G5_no_regression", {}).get("status") in ("PASS", "NOT_EVALUATED")
    if not synthetic:
        return "UNQUALIFIED"
    if g.get("G7_coverage", {}).get("real_device") != "validated":
        return "SYNTHETICALLY_HARDENED"
    if not ok("G4_latency"):
        return "DEVICE_VALIDATED"
    return "CERTIFICATION_CANDIDATE"  # CERTIFIED: only by signed independent reviewer disposition


def regression(new: sqlite3.Connection, base: sqlite3.Connection, cfg: dict) -> dict:
    """Non-inferiority per (profile, stratum, bin) cell on identical case ids, Holm-adjusted."""
    from analysis.statistics.stats import holm, noninferiority_p
    margin = cfg["noninferiority_margin_pp"] / 100
    b = {r[0]: r[1] for r in base.execute("SELECT case_id, success FROM cases")}
    cells: dict = {}
    for cid, s, p, sb, ok in new.execute("SELECT case_id, stratum, profile, sev_bin, success FROM cases"):
        if cid not in b:
            continue
        c = cells.setdefault((s, p, sb), [0, 0, 0])
        c[0] += 1; c[1] += b[cid]; c[2] += ok
    keys = [k for k, v in cells.items() if v[0] >= 400]
    pv = [noninferiority_p(cells[k][1], cells[k][0], cells[k][2], cells[k][0], margin) for k in keys]
    rej = holm(pv, cfg["alpha"])
    failing = [{"cell": "/".join(map(str, k)), "n": cells[k][0], "base": cells[k][1], "new": cells[k][2]} for k, r in zip(keys, rej) if not r]
    return {"status": "PASS" if not failing else "FAIL", "cells_tested": len(keys), "not_shown_noninferior": failing[:100], "margin_pp": cfg["noninferiority_margin_pp"]}
