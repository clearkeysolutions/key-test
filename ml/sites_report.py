"""Summarise `ckt_worker --ml-sites-compare`: classical vs site-model reading on identical
frames, from the true corners and through the full scan. Wrong payloads must be 0.

  python -m ml.sites_report RESULTS.jsonl [...] [--md OUT.md]
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from ml.compare_report import pct, quant, wilson


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+", type=Path)
    ap.add_argument("--md", type=Path)
    a = ap.parse_args()
    rows = [json.loads(l) for f in a.files for l in f.read_text().splitlines() if l.strip()]
    valid = [r for r in rows if r["valid"]]
    L = [f"# ML site model vs classical reading ({len(rows)} cases, identical frames)", "",
         "| | Classical | With site model |", "|---|---|---|"]
    out = {}
    for path in ("gtq", "scan"):
        sub = [r for r in valid if f"{path}_classical" in r]
        k = {arm: sum(r[f"{path}_{arm}"]["ok"] for r in sub) for arm in ("classical", "ml")}
        w = {arm: sum(r.get(f"{path}_{arm}", {}).get("wrong", 0) for r in rows) for arm in ("classical", "ml")}
        ms = {arm: [r[f"{path}_{arm}"]["ms"] for r in sub] for arm in ("classical", "ml")}
        gained = sum(1 for r in sub if r[f"{path}_ml"]["ok"] and not r[f"{path}_classical"]["ok"])
        lost = sum(1 for r in sub if r[f"{path}_classical"]["ok"] and not r[f"{path}_ml"]["ok"])
        by_ml = sum(r[f"{path}_ml"].get("ml", 0) for r in sub)
        label = "From the true corners" if path == "gtq" else "Full scan"
        L.append(f"| {label}: decoded | {pct(k['classical'] / max(1, len(sub)))} ({k['classical']}/{len(sub)}) | {pct(k['ml'] / max(1, len(sub)))} ({k['ml']}/{len(sub)}) |")
        L.append(f"| {label}: wrong payloads | {w['classical']} | {w['ml']} |")
        L.append(f"| {label}: time median / p95 (ms) | {quant(ms['classical'], .5):.0f} / {quant(ms['classical'], .95):.0f} | {quant(ms['ml'], .5):.0f} / {quant(ms['ml'], .95):.0f} |")
        L.append(f"| {label}: gained / lost, reads via the ML reading | | +{gained} / -{lost}, {by_ml} |")
        out[path] = {"n": len(sub), "decoded": k, "wrong": w, "gained": gained, "lost": lost, "ml_reads": by_ml,
                     "wilson95": {arm: wilson(k[arm], len(sub)) for arm in k}}
    L += ["", "| Stratum (from the true corners) | Classical | With site model |", "|---|---|---|"]
    for st in sorted({r["stratum"] for r in valid}):
        sub = [r for r in valid if r["stratum"] == st and "gtq_classical" in r]
        L.append(f"| {st} | {sum(r['gtq_classical']['ok'] for r in sub)}/{len(sub)} | {sum(r['gtq_ml']['ok'] for r in sub)}/{len(sub)} |")
    dense = [r for r in valid if r["v"] >= 25 and "gtq_classical" in r]
    L.append(f"| Dense v25-31 | {sum(r['gtq_classical']['ok'] for r in dense)}/{len(dense)} | {sum(r['gtq_ml']['ok'] for r in dense)}/{len(dense)} |")
    md = "\n".join(L) + "\n"
    print(md)
    if a.md:
        a.md.write_text(md); a.md.with_suffix(".json").write_text(json.dumps(out, indent=1, default=str))


if __name__ == "__main__":
    main()
