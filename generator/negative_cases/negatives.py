"""Hard negatives and near misses (SPEC §4: >= 10% of each applicable stratum).

Each kind's expected outcome comes from configs/strata.yaml; the worker
re-checks the pairing and refuses an inconsistent plan (INVALID_GENERATION).
"""
from __future__ import annotations

from generator.common import load_yaml

KINDS: dict = load_yaml("strata.yaml")["negatives"]["kinds"]
FEC_SINGLE_BLOCK = ("random_codeword", "heavy_corruption", "t_plus_one", "crc_forge", "splice", "mirror", "foreign_grid", "blank")
GENERIC = ("random_codeword", "heavy_corruption", "splice", "mirror", "foreign_grid", "blank")


def kinds_for(profile: str, stratum: str) -> tuple[str, ...]:
    # Profile 0 now has the full set: its block layout (spec 0.27) lets the independent RS
    # oracle build t+1 and CRC-forge codewords.
    base = FEC_SINGLE_BLOCK if profile in ("P0", "P0.1", "P1", "P2") else GENERIC
    if stratum == "S05":  # mirror/reflection rejection is S05's subject: half of its negatives
        return tuple(x for k in base for x in ([k, "mirror"] if k != "mirror" else []))
    return base


def negative_fields(profile: str, stratum: str, k: int, r) -> dict:
    kinds = kinds_for(profile, stratum)
    kind = kinds[k % len(kinds)]
    out = {"neg_kind": kind, "expected": KINDS[kind], "mode": "negative"}
    if kind == "heavy_corruption":
        out["neg_param"] = round(r.uniform(0.5, 0.75), 4)
    elif kind == "splice":
        out["neg_param"] = round(r.uniform(0.3, 0.7), 4)
    return out
