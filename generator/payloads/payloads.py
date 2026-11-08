"""Symbol and payload parameters per profile, balanced by deterministic cycling.

Profile 0 cycles every (version, ECC level, layout) combination the production
library supports (capacity > 0); Profile 3 cycles all seven tiers; payload
lengths are set as a fraction ("fill") of the selected capacity.
"""
from __future__ import annotations

import random

from generator.common import LEVELS, p0_capacity, p0_modules

PATTERNS = ("random", "zeros", "ones", "alt55", "altAA", "incrementing", "single_bit", "runs", "ascii", "url", "utf8")
DEFAULT_PATTERN_CYCLE = ("random",) * 14 + ("ascii", "ascii", "url", "url", "utf8", "runs")
P3_TIERS = (256, 384, 512, 768, 1024, 1536, 2048)


def p0_combos() -> list[tuple[int, str, str]]:
    out = []
    for layout in ("standard", "print"):
        for level in LEVELS:
            for v in range(1, 32):
                if p0_capacity(v, level, layout) > 0:
                    out.append((v, level, layout))
    return out


_P0 = None


def p0_combo(k: int) -> tuple[int, str, str]:
    global _P0
    if _P0 is None:
        _P0 = p0_combos()
        # Interleave so consecutive k sweep versions, levels and layouts together.
        _P0 = sorted(_P0, key=lambda c: ((c[0] * 7 + LEVELS.index(c[1]) * 3 + (c[2] == "print")) % len(_P0), c))
    return _P0[k % len(_P0)]


def symbol_params(profile: str, k: int, r: random.Random, fill: float | None = None, pattern: str | None = None) -> dict:
    """Returns case fields for the symbol plus geometry facts used by placement."""
    out: dict = {"profile": profile}
    if fill is None:
        fill = r.uniform(0.3, 1.0)
    if profile == "P1":
        out["payload_len"] = 16
        modules, canvas, field = 29, 31, 29
    elif profile == "P2":
        out["payload_len"] = max(1, min(200, round(fill * 200)))
        modules, canvas, field = 58, 31, 29
    elif profile == "P3":
        tier = P3_TIERS[k % len(P3_TIERS)]
        out["p3_tier"] = tier
        out["payload_len"] = max(1, min(tier, round(fill * tier)))
        modules, canvas, field = 232, 124, 116
    else:
        v, level, layout = p0_combo(k)
        cap = p0_capacity(v, level, layout)
        out.update({"p0_version": v, "p0_level": level, "p0_layout": layout})
        out["payload_len"] = max(1, min(cap, round(fill * cap)))
        modules, canvas, field = p0_modules(v), 31, 29
    out["payload_pattern"] = pattern or DEFAULT_PATTERN_CYCLE[(k // 4) % len(DEFAULT_PATTERN_CYCLE)]
    geo = {"modules": modules, "canvas_per_module": modules * canvas / field, "canvas": canvas, "field": field}
    return out, geo


def capacity_of(sym: dict) -> int:
    p = sym["profile"]
    if p == "P1":
        return 16
    if p == "P2":
        return 200
    if p == "P3":
        return int(sym["p3_tier"])
    return p0_capacity(sym["p0_version"], sym["p0_level"], sym["p0_layout"])
