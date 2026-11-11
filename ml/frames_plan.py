"""Plan for ML frame export (locator / quality network). Own master seed: never the
campaign's structured, adaptive or holdout seeds, so training data cannot overlap the
evaluation sets. Writes plain JSONL shards of `per_shard` cases.

  .venv/bin/python -m ml.frames_plan OUTDIR N [--per-shard 2000] [--tag v1]
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from generator.common import PROFILES
from generator.strata import STRATA, make_case


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("out", type=Path)
    ap.add_argument("n", type=int)
    ap.add_argument("--per-shard", type=int, default=2000)
    ap.add_argument("--tag", default="v1")
    a = ap.parse_args()
    master = f"CKEY-ML/frames/{a.tag}"
    a.out.mkdir(parents=True, exist_ok=True)
    strata = sorted(STRATA)
    shard, buf = 0, []
    for k in range(a.n):
        s, v, b = strata[k % 20], PROFILES[(k // 20) % len(PROFILES)], (k // 40) % 5
        mode = "negative" if k % 10 == 9 else ("sweep" if k % 3 == 0 else "joint")
        c = make_case("mlframes", master, s, v, b, mode, k, case_id=f"mlf-{a.tag}-{s}-{v}-b{b}-{k:07d}")
        c.pop("_notes", None)
        if c.get("second_code"):
            c["second_code"] = 0
            c["expected"] = "DECODE"
        buf.append(json.dumps(c, separators=(",", ":")))
        if len(buf) == a.per_shard:
            (a.out / f"shard-{shard:04d}.jsonl").write_text("\n".join(buf) + "\n"); shard += 1; buf = []
    if buf:
        (a.out / f"shard-{shard:04d}.jsonl").write_text("\n".join(buf) + "\n"); shard += 1
    print(f"{a.n} cases in {shard} shards under {a.out}")


if __name__ == "__main__":
    main()
