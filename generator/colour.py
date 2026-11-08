"""Colour pairs with an exact luminance contrast (linear-light, Rec. 709 / WCAG)."""
from __future__ import annotations

import random


def to_lin(c8: float) -> float:
    s = c8 / 255.0
    return s / 12.92 if s <= 0.04045 else ((s + 0.055) / 1.055) ** 2.4


def to_srgb8(l: float) -> int:
    l = min(1.0, max(0.0, l))
    s = 12.92 * l if l <= 0.0031308 else 1.055 * l ** (1 / 2.4) - 0.055
    return int(round(255 * s))


def luminance(rgb8) -> float:
    r, g, b = (to_lin(x) for x in rgb8)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def grey_pair_for_contrast(c: float, r: random.Random) -> tuple[list[int], list[int]]:
    """Dark and light greys whose linear reflectances have Michelson contrast c."""
    mean = r.uniform(0.15, 0.6)
    hi = mean * (1 + c)
    if hi > 0.9:
        mean *= 0.9 / hi
    d, l = mean * (1 - c), mean * (1 + c)
    return [to_srgb8(d)] * 3, [to_srgb8(l)] * 3


def colour_with_luminance(target: float, r: random.Random) -> list[int]:
    for _ in range(64):
        w = [r.random() + 0.05 for _ in range(3)]
        lum = 0.2126 * w[0] + 0.7152 * w[1] + 0.0722 * w[2]
        k = target / lum
        lin = [x * k for x in w]
        if max(lin) <= 1.0:
            return [to_srgb8(x) for x in lin]
    v = to_srgb8(target)
    return [v, v, v]


def colour_pair_for_ratio(ratio: float, r: random.Random) -> tuple[list[int], list[int]]:
    """(marks, background) of random hues with WCAG contrast ratio `ratio`; polarity random."""
    dark_max = 1.05 / ratio - 0.05
    ld = r.uniform(0.0, max(0.0, dark_max))
    ll = ratio * (ld + 0.05) - 0.05
    dark, light = colour_with_luminance(ld, r), colour_with_luminance(min(1.0, ll), r)
    return (dark, light) if r.random() < 0.5 else (light, dark)
