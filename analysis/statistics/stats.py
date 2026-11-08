"""Binomial uncertainty, clustering, and multiplicity (SPEC §9). Standard library only.

* wilson(k, n): Wilson score interval.
* clopper_pearson(k, n): exact interval via the regularized incomplete beta.
* zero_event_upper(n): exact one-sided 95% upper bound for 0 events (~3/n).
* cluster_effective_n: design effect from cluster (payload-family) variation,
  so correlated cases are not counted as independent evidence.
* newcombe_diff: CI for a difference of proportions (regression checks).
* holm: Holm-Bonferroni step-down adjustment.
"""
from __future__ import annotations

import math

Z95 = 1.959963984540054


def wilson(k: int, n: float, z: float = Z95) -> tuple[float, float]:
    if n <= 0:
        return (0.0, 1.0)
    p = k / n
    den = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / den
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / den
    return (max(0.0, centre - half), min(1.0, centre + half))


def _betacf(a: float, b: float, x: float) -> float:
    qab, qap, qam = a + b, a + 1, a - 1
    c, d = 1.0, 1 - qab * x / qap
    d = 1 / (d if abs(d) > 1e-300 else 1e-300)
    h = d
    for m in range(1, 300):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1 + aa * d; d = 1 / (d if abs(d) > 1e-300 else 1e-300)
        c = 1 + aa / c if abs(c) > 1e-300 else 1e300
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1 + aa * d; d = 1 / (d if abs(d) > 1e-300 else 1e-300)
        c = 1 + aa / c if abs(c) > 1e-300 else 1e300
        de = d * c
        h *= de
        if abs(de - 1) < 3e-14:
            break
    return h


def betainc(a: float, b: float, x: float) -> float:
    """Regularized incomplete beta I_x(a, b)."""
    if x <= 0:
        return 0.0
    if x >= 1:
        return 1.0
    lbt = math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b) + a * math.log(x) + b * math.log(1 - x)
    if x < (a + 1) / (a + b + 2):
        return math.exp(lbt) * _betacf(a, b, x) / a
    return 1 - math.exp(lbt) * _betacf(b, a, 1 - x) / b


def _beta_quantile(a: float, b: float, q: float) -> float:
    lo, hi = 0.0, 1.0
    for _ in range(200):
        mid = (lo + hi) / 2
        if betainc(a, b, mid) < q:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def clopper_pearson(k: int, n: int, alpha: float = 0.05) -> tuple[float, float]:
    if n <= 0:
        return (0.0, 1.0)
    lo = 0.0 if k == 0 else _beta_quantile(k, n - k + 1, alpha / 2)
    hi = 1.0 if k == n else _beta_quantile(k + 1, n - k, 1 - alpha / 2)
    return (lo, hi)


def zero_event_upper(n: int, alpha: float = 0.05) -> float:
    """Exact one-sided upper bound on the event rate when 0 of n trials had the event."""
    return 1 - alpha ** (1 / n) if n > 0 else 1.0


def one_sided_upper(k: int, n: int, alpha: float = 0.05) -> float:
    if n <= 0:
        return 1.0
    if k == 0:
        return zero_event_upper(n, alpha)
    return _beta_quantile(k + 1, n - k, 1 - alpha)


def cluster_effective_n(cluster_k: list[int], cluster_n: list[int]) -> tuple[float, float]:
    """(effective n, design effect) for a ratio estimator over clusters.

    Var(p_hat) = C/(C-1) * sum((k_c - p n_c)^2) / N^2 ; n_eff = p(1-p)/Var.
    """
    N = sum(cluster_n)
    C = len(cluster_n)
    if N == 0 or C < 2:
        return float(N), 1.0
    p = sum(cluster_k) / N
    if p in (0.0, 1.0):
        return float(N), 1.0
    var = C / (C - 1) * sum((k - p * n) ** 2 for k, n in zip(cluster_k, cluster_n)) / (N * N)
    if var <= 0:
        return float(N), 1.0
    n_eff = p * (1 - p) / var
    return min(float(N), n_eff), max(1.0, N / n_eff)


def newcombe_diff(k1: int, n1: int, k2: int, n2: int) -> tuple[float, float, float]:
    """Difference p2 - p1 with Newcombe's hybrid score CI (method 10)."""
    p1, p2 = k1 / n1, k2 / n2
    l1, u1 = wilson(k1, n1)
    l2, u2 = wilson(k2, n2)
    d = p2 - p1
    lo = d - math.sqrt((p2 - l2) ** 2 + (u1 - p1) ** 2)
    hi = d + math.sqrt((u2 - p2) ** 2 + (p1 - l1) ** 2)
    return d, lo, hi


def holm(pvalues: list[float], alpha: float = 0.05) -> list[bool]:
    """Holm step-down: returns reject flags in the input order."""
    order = sorted(range(len(pvalues)), key=lambda i: pvalues[i])
    reject = [False] * len(pvalues)
    m = len(pvalues)
    for rank, i in enumerate(order):
        if pvalues[i] <= alpha / (m - rank):
            reject[i] = True
        else:
            break
    return reject


def normal_sf(z: float) -> float:
    return 0.5 * math.erfc(z / math.sqrt(2))


def noninferiority_p(k_base: int, n_base: int, k_new: int, n_new: int, margin: float) -> float:
    """One-sided p-value for H0: p_new <= p_base - margin (Wald, unpooled)."""
    p1, p2 = k_base / n_base, k_new / n_new
    se = math.sqrt(max(1e-12, p1 * (1 - p1) / n_base + p2 * (1 - p2) / n_new))
    return normal_sf((p2 - p1 + margin) / se)


def quantile(values: list[float], q: float) -> float | None:
    if not values:
        return None
    v = sorted(values)
    pos = q * (len(v) - 1)
    lo = math.floor(pos)
    hi = min(len(v) - 1, lo + 1)
    return v[lo] + (v[hi] - v[lo]) * (pos - lo)
