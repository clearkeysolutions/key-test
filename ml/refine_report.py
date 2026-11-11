"""Summarise `ckt_worker --ml-refine-compare`: classical vs corner refiner on identical
frames, decoding from perturbed true corners (0.6 module per corner) and through the
full scan. Wrong payloads must be 0.

  python -m ml.refine_report RESULTS.jsonl [...] [--md OUT.md]
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from ml.compare_report import pct, quant


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+", type=Path)
    ap.add_argument("--md", type=Path)
    a = ap.parse_args()
    rows = [json.loads(l) for f in a.files for l in f.read_text().splitlines() if l.strip()]
    valid = [r for r in rows if r["valid"]]
    L = [f"# ML corner refiner vs classical ({len(rows)} cases, identical frames)", "", "| | Classical | With refiner |", "|---|---|---|"]
    out = {}
    for path, label in (("pq", "From corners 0.6 module off"), ("scan", "Full scan")):
        sub = [r for r in valid if f"{path}_classical" in r]
        k = {arm: sum(r[f"{path}_{arm}"]["ok"] for r in sub) for arm in ("classical", "ml")}
        w = {arm: sum(r.get(f"{path}_{arm}", {}).get("wrong", 0) for r in rows) for arm in ("classical", "ml")}
        ms = {arm: [r[f"{path}_{arm}"]["ms"] for r in sub] for arm in ("classical", "ml")}
        gained = sum(1 for r in sub if r[f"{path}_ml"]["ok"] and not r[f"{path}_classical"]["ok"])
        lost = sum(1 for r in sub if r[f"{path}_classical"]["ok"] and not r[f"{path}_ml"]["ok"])
        L.append(f"| {label}: decoded | {pct(k['classical'] / max(1, len(sub)))} ({k['classical']}/{len(sub)}) | {pct(k['ml'] / max(1, len(sub)))} ({k['ml']}/{len(sub)}) |")
        L.append(f"| {label}: wrong payloads | {w['classical']} | {w['ml']} |")
        L.append(f"| {label}: time median / p95 (ms) | {quant(ms['classical'], .5):.0f} / {quant(ms['classical'], .95):.0f} | {quant(ms['ml'], .5):.0f} / {quant(ms['ml'], .95):.0f} |")
        L.append(f"| {label}: gained / lost | | +{gained} / -{lost} |")
        out[path] = {"n": len(sub), "decoded": k, "wrong": w, "gained": gained, "lost": lost}
    L += ["", "| Stratum (full scan) | Classical | With refiner |", "|---|---|---|"]
    for st in sorted({r["stratum"] for r in valid}):
        sub = [r for r in valid if r["stratum"] == st]
        L.append(f"| {st} | {sum(r['scan_classical']['ok'] for r in sub)}/{len(sub)} | {sum(r['scan_ml']['ok'] for r in sub)}/{len(sub)} |")
    md = "\n".join(L) + "\n"
    print(md)
    if a.md:
        a.md.write_text(md); a.md.with_suffix(".json").write_text(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
