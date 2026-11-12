"""Summarise `ckt_worker --ml-compare` output: classical scan vs ML proposals vs ML + gate,
on identical frames. Wrong payloads must be 0 in every arm.

  python -m ml.compare_report RESULTS.jsonl [more.jsonl ...] [--md OUT.md]
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

ARMS = ("classical", "ml", "ml_gate", "ml_after")
LABELS = {"classical": "Classical", "ml": "ML proposals first", "ml_gate": "ML + gate", "ml_after": "ML after classical"}


def wilson(k, n, z=1.96):
    if n == 0:
        return (float("nan"), float("nan"))
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (c - h, c + h)


def pct(x):
    return f"{100 * x:.1f}%"


def quant(v, q):
    if not v:
        return float("nan")
    v = sorted(v)
    return v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))]


def summarise(rows):
    global ARMS
    ARMS = tuple(a for a in ARMS if rows and a in rows[0])
    out = {}
    valid = [r for r in rows if r["valid"]]
    recov = [r for r in valid if r.get("gtq")]
    for arm in ARMS:
        a = {}
        for name, sub in (("valid", valid), ("readable_from_true_corners", recov)):
            k = sum(r[arm]["ok"] for r in sub)
            a[name] = {"n": len(sub), "decoded": k, "rate": k / len(sub) if sub else float("nan"), "wilson95": wilson(k, len(sub))}
        a["wrong_payloads"] = sum(r[arm]["wrong"] for r in rows)
        ms = [r[arm]["ms"] for r in rows]
        ms_ok = [r[arm]["ms"] for r in valid if r[arm]["ok"]]
        ms_fail = [r[arm]["ms"] for r in rows if not r[arm]["ok"]]
        a["ms"] = {"p50": quant(ms, .5), "p95": quant(ms, .95), "decoded_p50": quant(ms_ok, .5), "not_decoded_p50": quant(ms_fail, .5), "not_decoded_p95": quant(ms_fail, .95)}
        a["decoded_by_ml_proposal"] = sum(r[arm].get("ml", 0) for r in rows)
        out[arm] = a
    # Per stratum, readable-from-true-corners
    strata = sorted({r["stratum"] for r in recov})
    out["by_stratum"] = {s: {arm: (sum(r[arm]["ok"] for r in recov if r["stratum"] == s), sum(1 for r in recov if r["stratum"] == s)) for arm in ARMS} for s in strata}
    # Dense: version >= 25
    dense = [r for r in recov if r["v"] >= 25]
    out["dense_v25_31"] = {arm: (sum(r[arm]["ok"] for r in dense), len(dense)) for arm in ARMS}
    out["gained_vs_classical"] = {arm: sum(1 for r in valid if r[arm]["ok"] and not r["classical"]["ok"]) for arm in ARMS[1:]}
    out["lost_vs_classical"] = {arm: sum(1 for r in valid if r["classical"]["ok"] and not r[arm]["ok"]) for arm in ARMS[1:]}
    return out


def markdown(s, n_rows):
    L = [f"# ML locator comparison ({n_rows} cases, identical frames)", "",
         "| | " + " | ".join(LABELS[a] for a in ARMS) + " |", "|---" * (len(ARMS) + 1) + "|"]
    def row(label, f):
        L.append(f"| {label} | " + " | ".join(f(s[a]) for a in ARMS) + " |")
    row("Valid symbols decoded", lambda a: f"{pct(a['valid']['rate'])} ({a['valid']['decoded']}/{a['valid']['n']})")
    row("Readable from true corners, decoded", lambda a: f"{pct(a['readable_from_true_corners']['rate'])} ({a['readable_from_true_corners']['decoded']}/{a['readable_from_true_corners']['n']})")
    row("Wrong payloads", lambda a: str(a["wrong_payloads"]))
    row("Time per frame, median / p95 (ms)", lambda a: f"{a['ms']['p50']:.0f} / {a['ms']['p95']:.0f}")
    row("Frames that decoded, median (ms)", lambda a: f"{a['ms']['decoded_p50']:.0f}")
    row("Frames that did not decode, median / p95 (ms)", lambda a: f"{a['ms']['not_decoded_p50']:.0f} / {a['ms']['not_decoded_p95']:.0f}")
    d = s["dense_v25_31"]
    L.append("| Dense (v25-31), readable, decoded | " + " | ".join(f"{d[a][0]}/{d[a][1]}" for a in ARMS) + " |")
    L += ["", "Gained vs classical: " + ", ".join(f"{LABELS[a]} {s['gained_vs_classical'][a]}" for a in ARMS[1:]) + ". "
          "Lost vs classical: " + ", ".join(f"{LABELS[a]} {s['lost_vs_classical'][a]}" for a in ARMS[1:]) + ".", "",
          "| Stratum (readable from true corners) | " + " | ".join(LABELS[a] for a in ARMS) + " |", "|---" * (len(ARMS) + 1) + "|"]
    for st, v in s["by_stratum"].items():
        L.append(f"| {st} | " + " | ".join(f"{v[a][0]}/{v[a][1]}" for a in ARMS) + " |")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+", type=Path)
    ap.add_argument("--md", type=Path)
    a = ap.parse_args()
    rows = [json.loads(l) for f in a.files for l in f.read_text().splitlines() if l.strip()]
    s = summarise(rows)
    md = markdown(s, len(rows))
    print(md)
    if a.md:
        a.md.write_text(md)
        a.md.with_suffix(".json").write_text(json.dumps(s, indent=1, default=str))


if __name__ == "__main__":
    main()
