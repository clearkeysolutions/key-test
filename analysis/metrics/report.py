"""Cohort reports (REPORT_1M, adaptive, holdout, regression): JSON + Markdown.

Every rate is reported with its denominator and a Wilson 95% interval;
safety counts with an exact one-sided 95% upper bound. Valid-symbol success
is shown both over all cases and over physically recoverable ones, and
adaptive results are never pooled with the structured baseline.
"""
from __future__ import annotations

import json
import sqlite3
import time
from collections import defaultdict
from pathlib import Path

from analysis.statistics.stats import cluster_effective_n, one_sided_upper, quantile, wilson
from generator.common import load_yaml, test_scale

STRATA = load_yaml("strata.yaml")["strata"]


def rate_row(k: int, n: int) -> dict:
    lo, hi = wilson(k, n)
    return {"n": n, "k": k, "rate": (k / n) if n else None, "wilson95": [lo, hi]}


def q(con: sqlite3.Connection, sql: str, args=()) -> list:
    return con.execute(sql, args).fetchall()


def cohort_report(con: sqlite3.Connection, cohort: str, plan_sets: list[str] | None = None) -> dict:
    where = "cohort=?"
    args: list = [cohort]
    if plan_sets:
        where += f" AND plan_set IN ({','.join('?' * len(plan_sets))})"
        args += plan_sets
    W = f"WHERE {where}"
    rep: dict = {"cohort": cohort, "plan_sets": plan_sets, "generated_at": time.time(), "test_scale": test_scale()}
    total = q(con, f"SELECT COUNT(*) FROM cases {W}", args)[0][0]
    rep["completed_cases"] = total
    rep["quota"] = {
        "by_stratum": dict(q(con, f"SELECT stratum, COUNT(*) FROM cases {W} GROUP BY stratum", args)),
        "by_stratum_profile": {f"{s}/{p}": n for s, p, n in q(con, f"SELECT stratum, variant, COUNT(*) FROM cases {W} GROUP BY stratum, variant", args)},
        "by_mode": dict(q(con, f"SELECT mode, COUNT(*) FROM cases {W} GROUP BY mode", args)),
        "by_pool": dict(q(con, f"SELECT COALESCE(pool,''), COUNT(*) FROM cases {W} GROUP BY pool", args)),
    }
    # ---- safety: accepted wrong payloads
    wrong = q(con, f"SELECT variant, COUNT(*), SUM(wrong_payload) FROM cases {W} GROUP BY variant", args)
    rep["safety"] = {
        "wrong_payloads_total": sum(int(w or 0) for _, _, w in wrong),
        "by_profile": {p: {"cases": n, "wrong": int(w or 0), "upper95_rate": one_sided_upper(int(w or 0), n)} for p, n, w in wrong},
        "upper95_rate_all": one_sided_upper(sum(int(w or 0) for _, _, w in wrong), total) if total else None,
        "by_neg_kind": {k: {"cases": n, "wrong": int(w or 0)} for k, n, w in q(con, f"SELECT neg_kind, COUNT(*), SUM(wrong_payload) FROM cases {W} GROUP BY neg_kind", args)},
        "wrong_cases": [dict(zip(("case_id", "stratum", "profile", "neg_kind", "accepted_match", "image_uri", "trace_uri"), r))
                        for r in q(con, f"SELECT case_id, stratum, profile, neg_kind, accepted_match, image_uri, trace_uri FROM cases {W} AND wrong_payload=1 LIMIT 500", args)],
        "note": "Zero observed is a release gate, not proof of impossibility; bounds assume independent trials and are optimistic under correlation.",
    }
    # ---- valid-symbol decode success
    V = f"{W} AND neg_kind='none' AND expected_outcome IN ('DECODE','DECODE_ANY_OF')"
    def table(group_sql: str, extra: str = ""):
        out = {}
        for row in q(con, f"SELECT {group_sql}, COUNT(*), SUM(success) FROM cases {V} {extra} GROUP BY {group_sql}", args):
            key = "/".join(str(x) for x in row[:-2])
            out[key] = rate_row(int(row[-1] or 0), row[-2])
        return out
    rep["valid"] = {
        "overall": table("'all'"),
        "overall_recoverable": table("'all'", "AND phys_class='RECOVERABLE'"),
        "by_profile": table("variant"),
        "by_density": table("CASE WHEN p0_version<=8 THEN 'v1-8' WHEN p0_version<=16 THEN 'v9-16' WHEN p0_version<=24 THEN 'v17-24' ELSE 'v25-31' END"),
        "by_variant_density": table("variant, CASE WHEN p0_version<=8 THEN 'v1-8' WHEN p0_version<=16 THEN 'v9-16' WHEN p0_version<=24 THEN 'v17-24' ELSE 'v25-31' END"),
        "by_variant_level": table("variant, p0_level"),
        "by_stratum": table("stratum"),
        "by_stratum_recoverable": table("stratum", "AND phys_class='RECOVERABLE'"),
        "by_stratum_profile": table("stratum, variant"),
        "by_stratum_bin": table("stratum, sev_bin"),
        "by_stratum_profile_bin": table("stratum, variant, sev_bin"),
        "by_phys_class": table("phys_class"),
        "by_device": table("camera_model"),
        "by_substrate": table("substrate"),
    }
    # Cluster-robust overall interval (payload families share a payload).
    fams = q(con, f"SELECT payload_family, COUNT(*), SUM(success) FROM cases {V} GROUP BY payload_family", args)
    if fams:
        n_eff, deff = cluster_effective_n([int(k or 0) for _, _, k in fams], [n for _, n, _ in fams])
        k = sum(int(k or 0) for _, _, k in fams); n = sum(n for _, n, _ in fams)
        p = k / n
        lo, hi = wilson(round(p * n_eff), n_eff)
        rep["valid"]["overall_cluster_robust"] = {"n": n, "k": k, "rate": p, "n_eff": n_eff, "design_effect": deff, "wilson95_neff": [lo, hi]}
    # ---- failure mechanisms
    mech = defaultdict(dict)
    for s, reason, n in q(con, f"SELECT stratum, failure_reason, COUNT(*) FROM cases {V} AND success=0 GROUP BY stratum, failure_reason", args):
        mech[s][reason] = n
    rep["failure_mechanisms"] = dict(mech)
    tri = q(con, f"SELECT stratum, gt_quad_decodes, COUNT(*) FROM cases {V} AND success=0 AND gt_quad_decodes>=0 GROUP BY stratum, gt_quad_decodes", args)
    triage = defaultdict(lambda: {"localization_limited": 0, "sampling_or_decode_limited": 0})
    for s, g, n in tri:
        triage[s]["localization_limited" if g == 1 else "sampling_or_decode_limited"] += n
    rep["triage_gt_quad"] = dict(triage)
    rep["triage_note"] = ("localization_limited: the production decoder reads the frame when given the true field corners, "
                          "so the failure is in border finding / quad refinement; otherwise sampling or decoding is limiting.")
    # ---- negatives
    neg = {}
    for kind, prof, n, rej, wrongs in q(con, f"SELECT neg_kind, variant, COUNT(*), SUM(observed_outcome='NO_READ'), SUM(wrong_payload) FROM cases {W} AND neg_kind!='none' GROUP BY neg_kind, variant", args):
        neg[f"{kind}/{prof}"] = {"cases": n, "rejected": int(rej or 0), "wrong": int(wrongs or 0), "reject_rate": (rej or 0) / n}
    rep["negatives"] = neg
    # ---- physical classes
    rep["phys_classes"] = {c: {"cases": n, "success": int(k or 0)} for c, n, k in q(con, f"SELECT phys_class, COUNT(*), SUM(success) FROM cases {V} GROUP BY phys_class", args)}
    # ---- latency, memory (host-indicative)
    lat = {}
    for prof, fw in q(con, f"SELECT DISTINCT variant, frame_w FROM cases {W} AND frame_count=1", args):
        vals = [r[0] for r in q(con, f"SELECT decode_ms_first FROM cases {W} AND variant=? AND frame_w=? AND decode_ms_first IS NOT NULL", args + [prof, fw])]
        lat[f"{prof}/{fw}w"] = {"n": len(vals), "p50": quantile(vals, .5), "p95": quantile(vals, .95), "p99": quantile(vals, .99), "max": max(vals) if vals else None}
    tts = [r[0] for r in q(con, f"SELECT tts_ms FROM cases {V} AND frames_planned>1 AND success=1", args)]
    rep["latency_host_ms"] = {"single_frame_decode": lat, "sequence_time_to_success": {"n": len(tts), "p50": quantile(tts, .5), "p95": quantile(tts, .95)},
                              "note": "Host (server) timings of the C++ core; indicative only. Gate G4 requires the reference iOS device."}
    mem = [r[0] / 1048576 for r in q(con, f"SELECT peak_memory FROM cases {W} AND peak_memory IS NOT NULL", args)]
    rep["memory_mb"] = {"p50": quantile(mem, .5), "p99": quantile(mem, .99), "max": max(mem) if mem else None,
                        "note": "Per-case peak heap of the worker process (reader + simulator)."}
    faults = q(con, f"SELECT failure_reason, COUNT(*) FROM cases {W} AND failure_reason LIKE 'READER_%' GROUP BY failure_reason", args)
    rep["reader_faults"] = dict(faults)
    # ---- oracle checks
    cc = [r[0] for r in q(con, f"SELECT crosscheck FROM cases {W} AND crosscheck >= 0", args)]
    rs = dict(q(con, f"SELECT rs_verified, COUNT(*) FROM cases {W} AND rs_verified >= 0 GROUP BY rs_verified", args))
    rep["oracle"] = {"renderer_crosscheck": {"n": len(cc), "max": max(cc) if cc else None, "p99": quantile(cc, .99)},
                     "independent_rs": {"verified": rs.get(1, 0), "mismatch": rs.get(0, 0)}}
    # ---- coverage
    rep["coverage"] = {
        "devices": dict(q(con, f"SELECT camera_model, COUNT(*) FROM cases {W} GROUP BY camera_model", args)),
        "substrates": dict(q(con, f"SELECT substrate, COUNT(*) FROM cases {W} GROUP BY substrate", args)),
        "p0_versions": dict(q(con, f"SELECT p0_version, COUNT(*) FROM cases {W} AND profile='P0' GROUP BY p0_version", args)),
        "p0_levels_layouts": {f"{a}/{b}": n for a, b, n in q(con, f"SELECT p0_level, p0_layout, COUNT(*) FROM cases {W} AND profile='P0' GROUP BY p0_level, p0_layout", args)},
        "p3_tiers": dict(q(con, f"SELECT p3_tier, COUNT(*) FROM cases {W} AND profile='P3' GROUP BY p3_tier", args)),
        "patterns": dict(q(con, f"SELECT payload_pattern, COUNT(*) FROM cases {W} GROUP BY payload_pattern", args)),
        "ppm_min_bins": dict(q(con, f"SELECT CAST(MIN(ppm_min, 20) AS INTEGER), COUNT(*) FROM cases {V} GROUP BY 1", args)),
    }
    return rep


def fmt_rate(r: dict) -> str:
    if not r or not r["n"]:
        return "—"
    lo, hi = r["wilson95"]
    return f"{100 * r['rate']:.2f}% [{100 * lo:.2f}, {100 * hi:.2f}] (n={r['n']:,})"


def markdown(rep: dict, title: str) -> str:
    L = [f"# {title}", "", f"Cohort `{rep['cohort']}` · {rep['completed_cases']:,} completed cases · generated {time.strftime('%Y-%m-%d %H:%M UTC', time.gmtime(rep['generated_at']))}", "",
         "> Simulated, host-side evidence about the production CKey core. Not a certification; see `certification/qualification/README.md`.", ""]
    if rep.get("test_scale", 1) != 1:
        L += [f"> **TEST SCALE 1/{rep['test_scale']}: pipeline exercise only. These numbers are not campaign evidence.**", ""]
    s = rep["safety"]
    L += ["## Safety: accepted wrong payloads", "", f"**Total wrong payloads: {s['wrong_payloads_total']}**"
          + (f" · one-sided 95% upper bound on the rate: {s['upper95_rate_all']:.2e} (assumes independence)" if s.get("upper95_rate_all") is not None else ""), "",
          "| Variant | Cases | Wrong | 95% upper rate |", "|---|---:|---:|---:|"]
    for p, v in sorted(s["by_profile"].items()):
        L.append(f"| {p} | {v['cases']:,} | {v['wrong']} | {v['upper95_rate']:.2e} |")
    L += ["", "## Valid-symbol decode success", "", f"* All valid cases: {fmt_rate(rep['valid']['overall'].get('all'))}",
          f"* Physically recoverable only: {fmt_rate(rep['valid']['overall_recoverable'].get('all'))}"]
    cr = rep["valid"].get("overall_cluster_robust")
    if cr:
        L.append(f"* Cluster-robust (payload families): n_eff {cr['n_eff']:,.0f}, design effect {cr['design_effect']:.2f}, 95% [{100 * cr['wilson95_neff'][0]:.2f}, {100 * cr['wilson95_neff'][1]:.2f}]%")
    L += ["", "| Stratum | Name | All valid | Recoverable only |", "|---|---|---|---|"]
    for st in sorted(STRATA):
        L.append(f"| {st} | {STRATA[st]['name']} | {fmt_rate(rep['valid']['by_stratum'].get(st))} | {fmt_rate(rep['valid']['by_stratum_recoverable'].get(st))} |")
    L += ["", "### By variant (P0 = Format 0.27 standard, P0.1 = Format 0.28 print)", "", "| Variant | Success |", "|---|---|"]
    for p, r in sorted(rep["valid"]["by_profile"].items()):
        L.append(f"| {p} | {fmt_rate(r)} |")
    L += ["", "### By density (P0 version band) and variant", "", "| Variant / band | Success |", "|---|---|"]
    for p, r in sorted(rep["valid"]["by_variant_density"].items()):
        L.append(f"| {p} | {fmt_rate(r)} |")
    L += ["", "### By stratum and severity bin (0 = easiest)", "", "| Stratum | b0 | b1 | b2 | b3 | b4 |", "|---|---|---|---|---|---|"]
    for st in sorted(STRATA):
        cells = []
        for b in range(5):
            r = rep["valid"]["by_stratum_bin"].get(f"{st}/{b}")
            cells.append(f"{100 * r['rate']:.1f}%" if r and r["n"] else "—")
        L.append(f"| {st} | " + " | ".join(cells) + " |")
    L += ["", "## Failure mechanisms (valid symbols)", "", "| Stratum | Mechanisms | GT-quad triage (localization / sampling-decode) |", "|---|---|---|"]
    for st in sorted(STRATA):
        m = rep["failure_mechanisms"].get(st, {})
        t = rep["triage_gt_quad"].get(st, {})
        L.append(f"| {st} | " + ", ".join(f"{k} {v}" for k, v in sorted(m.items(), key=lambda kv: -kv[1])) + f" | {t.get('localization_limited', 0)} / {t.get('sampling_or_decode_limited', 0)} |")
    L += ["", rep["triage_note"], "", "## Hard negatives", "", "| Kind / profile | Cases | Rejected | Wrong |", "|---|---:|---:|---:|"]
    for k, v in sorted(rep["negatives"].items()):
        L.append(f"| {k} | {v['cases']:,} | {v['rejected']:,} | {v['wrong']} |")
    L += ["", "## Physical recoverability (valid symbols)", "", "| Class | Cases | Success |", "|---|---:|---:|"]
    for c, v in sorted(rep["phys_classes"].items()):
        L.append(f"| {c} | {v['cases']:,} | {v['success']:,} |")
    L += ["", "## Host latency (indicative only; not gate G4)", "", "| Profile / frame | n | P50 ms | P95 ms | P99 ms |", "|---|---:|---:|---:|---:|"]
    for k, v in sorted(rep["latency_host_ms"]["single_frame_decode"].items()):
        if v["n"]:
            L.append(f"| {k} | {v['n']:,} | {v['p50']:.1f} | {v['p95']:.1f} | {v['p99']:.1f} |")
    m = rep["memory_mb"]
    L += ["", f"Peak per-case heap: P50 {m['p50'] or 0:.0f} MB, P99 {m['p99'] or 0:.0f} MB, max {m['max'] or 0:.0f} MB. Reader faults: {rep['reader_faults'] or 'none'}.", "",
          "## Oracle checks", "", f"* Renderer cross-check (S01): n={rep['oracle']['renderer_crosscheck']['n']:,}, max disagreement {rep['oracle']['renderer_crosscheck']['max']}",
          f"* Independent RS vs production parity: {rep['oracle']['independent_rs']['verified']:,} verified, {rep['oracle']['independent_rs']['mismatch']} mismatches", "",
          "## Quota", "", "```", json.dumps(rep["quota"]["by_stratum"], indent=1), "```", ""]
    return "\n".join(L)


def write(rep: dict, out_dir: Path, name: str, title: str) -> tuple[Path, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    j, m = out_dir / f"{name}.json", out_dir / f"{name}.md"
    j.write_text(json.dumps(rep, indent=1, sort_keys=True, default=str))
    m.write_text(markdown(rep, title))
    return j, m
