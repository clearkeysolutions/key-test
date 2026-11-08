"""Adaptive campaign (SPEC §7): weakness scoring, allocation, and the three pools.

Inputs are the FROZEN structured results; the weights and rules come from
configs/adaptive.yaml, whose hash is recorded in the REPORT_1M freeze before
this module may run. Physically UNRECOVERABLE cases are excluded from the
no-read rate so budget is not spent re-proving the impossible.

Pools (exactly 500,000):
  failure_targeted 350,000  per (stratum, profile, bin), >= 5,000 each, <= 20% of the pool of 500k
  cross_condition   50,000  worst conditions recombined pairwise across profiles/devices/payloads
  boundary_search  100,000  bisection on the primary factor + minimization + perturbation fill
"""
from __future__ import annotations

import json
import math
import sqlite3
from collections import defaultdict
from pathlib import Path

from analysis.statistics.stats import quantile, wilson
from generator.common import PROFILES, load_yaml, rng_for, scaled
from generator.strata import STRATA, make_case

ACFG = load_yaml("adaptive.yaml")
# Test scale (CKT_TEST_SCALE) shrinks every pool proportionally; 1 in a real campaign.
ACFG["total"] = scaled(ACFG["total"])
ACFG["pools"] = {k: scaled(v) for k, v in ACFG["pools"].items()}
ACFG["failure_targeted"]["min_per_bin"] = scaled(ACFG["failure_targeted"]["min_per_bin"])
ACFG["boundary_search"]["minimize_tracks"] = scaled(ACFG["boundary_search"]["minimize_tracks"], 0)
MASTER = ACFG["master_seed"]
STRUCT_MASTER = load_yaml("strata.yaml")["master_seed"]
MODE_SPLIT = (("negative", 0.10), ("sweep", 0.44), ("joint", 0.46))

# Harder direction and physical hard limit of each continuous primary factor.
HARDER = {"S01": ("-", -1.0), "S02": ("+", 1.0), "S03": ("-", 0.8), "S04": ("+", 80.0), "S06": ("+", 2.0), "S07": ("+", 8.0),
          "S08": ("-", 3.0), "S09": ("-", 0.02), "S10": ("+", 0.8), "S11": ("+", 0.8), "S12": ("+", 0.6), "S13": ("-", 1.0),
          "S14": ("-", 2.0), "S15": ("-", 1.05), "S17": ("+", 3.0), "S18": ("-", 0.5), "S20": ("+", 2.5)}
LOG_FACTORS = {"S03", "S08", "S14", "S15", "S18"}
SECONDARY_RESETS = [
    ("motion", {"motion_px": 0, "rs_ratio": 0, "rot_blur_deg": 0, "zoom_blur": 0}), ("defocus", {"defocus_gauss_px": 0, "defocus_disc_px": 0}),
    ("glare", {"glare_spots": 0, "veil": 0}), ("shadow_gradient", {"shadows": 0, "grad": 0}), ("jpeg", {"jpeg_q": 0}),
    ("clutter", {"clutter": 0.1, "distractors": 0}), ("occlusion", {"occl_kind": "none", "occl_frac": 0}),
    ("perspective", {"yaw": 0, "pitch": 0, "bend_radius_mm": 0}), ("substrate", {"substrate": "ideal"}),
    ("colour", {"color_mode": "mono", "bayer": 0}), ("light_level", {"illum": 1.2}), ("isp", {"tone": 0, "video_range": 0, "rescale": 1.0}),
]


# ---------------------------------------------------------------- scoring
def est_t_bytes(profile: str, ecc: float, payload_len: int) -> float:
    cw = {"P1": 77, "P2": 255, "P3": 2550}.get(profile)
    if cw is None:  # P0: approximate codeword size from payload and level
        cw = payload_len / max(0.2, 1 - 2 * ecc) + 10
    return max(1.0, ecc * cw)


def weakness_scores(con: sqlite3.Connection) -> list[dict]:
    w = ACFG["failure_targeted"]["weights"]
    budget = ACFG["failure_targeted"]["latency_budget_ms"]
    rows = con.execute("""SELECT stratum, profile, sev_bin, success, wrong_payload, decode_ms_first, localization_error_modules,
                                 FEC_corrections, ecc_fraction, payload_len, failure_reason, phys_class
                          FROM cases WHERE cohort='structured' AND neg_kind='none' AND expected_outcome IN ('DECODE','DECODE_ANY_OF')""").fetchall()
    wrong_any = defaultdict(int)
    for s, p, b, w_ in con.execute("SELECT stratum, profile, sev_bin, SUM(wrong_payload) FROM cases WHERE cohort='structured' GROUP BY stratum, profile, sev_bin"):
        wrong_any[(s, p, b)] = int(w_ or 0)
    cells = defaultdict(lambda: {"n": 0, "ok": 0, "lat": [], "loc": [], "margin": [], "mech": defaultdict(int)})
    for s, p, b, ok, wr, ms, loc, fec, ecc, plen, reason, cls in rows:
        c = cells[(s, p, b)]
        if cls == "UNRECOVERABLE":
            continue
        c["n"] += 1
        c["ok"] += int(ok)
        if ms is not None:
            c["lat"].append(ms)
        if loc is not None:
            c["loc"].append(loc)
        if ok:
            c["margin"].append(min(1.0, (fec or 0) / est_t_bytes(p, ecc or 0.1, plen or 1)))
        else:
            c["mech"][reason] += 1
    out = []
    for (s, p, b), c in cells.items():
        n = c["n"]
        noread = 1 - c["ok"] / n if n else 0.0
        lo, hi = wilson(c["ok"], n) if n else (0, 1)
        p95 = quantile(c["lat"], .95) or 0.0
        loc = quantile(c["loc"], .5) or 0.0
        margin = sum(c["margin"]) / len(c["margin"]) if c["margin"] else 0.0
        comp = {"noread": noread, "wrong": 1.0 if wrong_any[(s, p, b)] else 0.0, "latency": max(0.0, p95 / budget - 1),
                "geometry": min(1.0, loc / 0.5), "uncertainty": hi - lo, "margin": margin}
        score = (w["w_noread"] * comp["noread"] + w["w_wrong"] * comp["wrong"] + w["w_latency"] * comp["latency"]
                 + w["w_geometry"] * comp["geometry"] + w["w_uncertainty"] * comp["uncertainty"] + w["w_margin"] * comp["margin"])
        out.append({"stratum": s, "profile": p, "sev_bin": b, "n_recoverable": n, "score": score, "components": comp,
                    "wrong_payloads": wrong_any[(s, p, b)], "mechanisms": dict(c["mech"])})
    out.sort(key=lambda r: (-(r["wrong_payloads"] > 0), -r["score"], r["stratum"], r["profile"], r["sev_bin"]))
    return out


def allocate(scores: list[dict], total: int, min_per: int, cap: int) -> list[tuple[dict, int]]:
    """Proportional-to-score allocation with a floor and a cap, summing exactly to `total`."""
    k = max(1, min(len(scores), total // min_per))
    chosen = scores[:k]
    alloc = {i: min_per for i in range(k)}
    left = total - min_per * k
    weights = {i: max(1e-9, chosen[i]["score"]) for i in range(k)}
    while left > 0:
        open_ = [i for i in range(k) if alloc[i] < cap]
        if not open_:
            break
        wsum = sum(weights[i] for i in open_)
        given = 0
        for i in open_:
            add = min(cap - alloc[i], int(left * weights[i] / wsum))
            alloc[i] += add
            given += add
        if given == 0:  # distribute the remainder one by one, highest score first
            for i in sorted(open_, key=lambda i: -weights[i]):
                if left - given <= 0:
                    break
                if alloc[i] < cap:
                    alloc[i] += 1
                    given += 1
        left -= given
    return [(chosen[i], alloc[i]) for i in range(k)]


# ---------------------------------------------------------------- pools
def failure_targeted(scores: list[dict]) -> list[dict]:
    cfg = ACFG["failure_targeted"]
    pool = ACFG["pools"]["failure_targeted"]
    cap = int(cfg["max_bin_fraction_of_pool"] * ACFG["total"])
    cases = []
    for cell, n in allocate(scores, pool, cfg["min_per_bin"], cap):
        s, p, b = cell["stratum"], cell["profile"], cell["sev_bin"]
        counts = [round(n * f) for _, f in MODE_SPLIT]
        counts[-1] += n - sum(counts)
        for (mode, _), m in zip(MODE_SPLIT, counts):
            for k in range(m):
                c = make_case("adaptive", MASTER, s, p, b, mode, k, case_id=f"adaptive-ft-{s}-{p}-b{b}-{mode[:3]}-{k:06d}")
                c["pool"] = "failure_targeted"
                cases.append(c)
    assert len(cases) == pool, len(cases)
    return cases


def cross_condition(scores: list[dict]) -> list[dict]:
    pool = ACFG["pools"]["cross_condition"]
    agg = defaultdict(float)
    for r in scores:
        if r["stratum"] not in ("S01", "S02", "S20"):  # S02 (payload fill) only acts as a primary factor
            agg[(r["stratum"], r["sev_bin"])] += r["score"]
    top = [k for k, _ in sorted(agg.items(), key=lambda kv: -kv[1]) if True]
    seen, cond = set(), []
    for (s, b) in top:  # at most one bin per stratum so pairs combine distinct factors
        if s in seen:
            continue
        seen.add(s)
        cond.append((s, b))
        if len(cond) == ACFG["cross_condition"]["top_conditions"]:
            break
    pairs = [(a, c) for i, a in enumerate(cond) for c in cond[i + 1:]]
    pairs = pairs[:pool] if pool < len(pairs) else pairs
    per = pool // len(pairs)
    cases = []
    for pi, ((sa, ba), (sb, bb)) in enumerate(pairs):
        n = per + (1 if pi < pool - per * len(pairs) else 0)
        for k in range(n):
            prof = PROFILES[k % 4]
            r = rng_for(MASTER, "cc-sev", sa, ba, sb, bb, k)
            lo, hi = sorted(STRATA[sb]["bins"][bb])
            xb = r.uniform(lo, hi)
            c = make_case("adaptive", MASTER, sa, prof, ba, "joint", k + 1000 * pi, extra_factors=[(sb, xb)],
                          case_id=f"adaptive-cc-{sa}b{ba}x{sb}b{bb}-{prof}-{k:06d}")
            c["pool"] = "cross_condition"
            c["factor"] = f"{STRATA[sa]['factor']}+{STRATA[sb]['factor']}"
            cases.append(c)
    assert len(cases) == pool, len(cases)
    return cases


# ---------------------------------------------------------------- boundary search
def factor_range(stratum: str) -> tuple[float, float]:
    """(easy end, hard limit) of the primary factor's value."""
    direction, limit = HARDER[stratum]
    vals = [v for b in STRATA[stratum]["bins"] for v in b]
    easy = min(vals) if direction == "+" else max(vals)
    return easy, limit


def interp(stratum: str, a: float, b: float, t: float) -> float:
    if stratum in LOG_FACTORS and a > 0 and b > 0:
        return math.exp(math.log(a) + t * (math.log(b) - math.log(a)))
    return a + t * (b - a)


def parse_case_id(cid: str) -> tuple:
    # structured-S03-P2-b1-swe-00123
    _, s, p, b, m, k = cid.split("-")
    mode = {"swe": "sweep", "joi": "joint", "neg": "negative"}[m]
    return s, p, int(b[1:]), mode, int(k)


def init_tracks(con: sqlite3.Connection, scores: list[dict], n_tracks: int) -> list[dict]:
    eligible = [r for r in scores if r["stratum"] in HARDER]
    alloc = allocate(eligible, n_tracks, 1, n_tracks)
    tracks = []
    for cell, n in alloc:
        s, p, b = cell["stratum"], cell["profile"], cell["sev_bin"]
        rows = con.execute("""SELECT case_id, success FROM cases WHERE cohort='structured' AND stratum=? AND profile=? AND sev_bin=?
                              AND neg_kind='none' AND mode IN ('sweep','joint') AND phys_class!='UNRECOVERABLE' ORDER BY success, case_id""", (s, p, b)).fetchall()
        for i in range(min(n, len(rows))):
            cid = rows[i][0]
            _, _, bb, mode, k = parse_case_id(cid)
            easy, hard = factor_range(s)
            tracks.append({"id": len(tracks), "stratum": s, "profile": p, "sev_bin": bb, "mode": mode, "k": k, "base_case": cid,
                           "pass": easy, "fail": hard, "status": "init", "history": []})
    return tracks


def probe_cases(tr: dict, value: float, round_name: str, seeds: int, overrides: dict | None = None, tag: str = "") -> list[dict]:
    out = []
    for j in range(seeds):
        c = make_case("adaptive", MASTER, tr["stratum"], tr["profile"], tr["sev_bin"], tr["mode"], tr["k"], severity=value,
                      seed_labels=(round_name, tr["id"], tag, j), params_from=(STRUCT_MASTER, "structured"),
                      case_id=f"adaptive-bs-{round_name}-t{tr['id']:05d}{tag}-s{j}")
        if overrides:
            c.update(overrides)
        c["pool"] = "boundary_search"
        out.append(c)
    return out


class Boundary:
    """Persistent boundary-search state (campaign/state/boundary.json)."""

    def __init__(self, path: Path):
        self.path = path
        self.state = json.loads(path.read_text()) if path.exists() else None

    def save(self) -> None:
        self.path.write_text(json.dumps(self.state, indent=1))

    def start(self, con: sqlite3.Connection, scores: list[dict]) -> None:
        cfg = ACFG["boundary_search"]
        per_track = 2 * cfg["seeds_per_probe"] + cfg["rounds"] * cfg["seeds_per_probe"]
        minimize = cfg["minimize_tracks"] * len(SECONDARY_RESETS)
        n_tracks = max(1, (ACFG["pools"]["boundary_search"] - minimize) // per_track)
        self.state = {"tracks": init_tracks(con, scores, n_tracks), "round": 0, "spent": 0, "budget": ACFG["pools"]["boundary_search"],
                      "phase": "init", "rounds_done": []}
        self.save()

    def next_round(self) -> tuple[str, list[dict]] | None:
        st = self.state
        cfg = ACFG["boundary_search"]
        seeds = cfg["seeds_per_probe"]
        tr_all = st["tracks"]
        if st["phase"] == "init":
            name = "r00"
            cases = []
            for t in tr_all:
                cases += probe_cases(t, t["pass"], name, seeds, tag="e") + probe_cases(t, t["fail"], name, seeds, tag="h")
            return name, cases
        if st["phase"] == "bisect" and st["round"] <= cfg["rounds"]:
            name = f"r{st['round']:02d}"
            cases = []
            for t in tr_all:
                if t["status"] == "bisect":
                    cases += probe_cases(t, interp(t["stratum"], t["pass"], t["fail"], 0.5), name, seeds)
            if cases:
                return name, cases
            st["phase"] = "minimize"
        if st["phase"] == "minimize":
            name = "min"
            ranked = sorted([t for t in tr_all if t["status"] in ("bisect", "bounded", "fails_at_easy")], key=lambda t: self.difficulty(t))
            cases = []
            for t in ranked[:cfg["minimize_tracks"]]:
                for tag, ov in SECONDARY_RESETS:
                    cases += probe_cases(t, t["fail"], name, 1, overrides=ov, tag=f"-{tag}")
            if cases:
                return name, cases
            st["phase"] = "fill"
        if st["phase"] == "fill":
            left = st["budget"] - st["spent"]
            if left <= 0:
                return None
            ranked = sorted([t for t in tr_all if t["status"] in ("bounded", "fails_at_easy", "bisect")], key=lambda t: self.difficulty(t)) or tr_all
            cases = []
            j = 0
            while len(cases) < left:
                t = ranked[j % len(ranked)]
                c = probe_cases(t, t["fail"], "fill", 1, tag=f"-p{j // len(ranked)}")[0]
                cases.append(c)
                j += 1
            return "fill", cases
        return None

    def difficulty(self, t: dict) -> float:
        """Normalized position of the failure boundary in [0,1]: lower = fails under milder conditions."""
        easy, hard = factor_range(t["stratum"])
        if hard == easy:
            return 1.0
        return abs(t["fail"] - easy) / abs(hard - easy)

    def update(self, name: str, results: dict[str, dict]) -> None:
        """Apply a finished round's results (case_id -> result row)."""
        st = self.state
        seeds = ACFG["boundary_search"]["seeds_per_probe"]

        def fails(prefix: str, n: int) -> bool | None:
            got = [results.get(f"{prefix}-s{j}") for j in range(n)]
            got = [g for g in got if g is not None]
            if not got:
                return None
            return sum(1 for g in got if not g["success"]) * 2 > len(got)

        for t in st["tracks"]:
            base = f"adaptive-bs-{name}-t{t['id']:05d}"
            if name == "r00":
                fe, fh = fails(base + "e", seeds), fails(base + "h", seeds)
                t["history"].append({"round": name, "easy_fails": fe, "hard_fails": fh})
                if fe:
                    t["status"], t["fail"] = "fails_at_easy", t["pass"]
                elif fh is False:
                    t["status"] = "robust_to_limit"
                else:
                    t["status"] = "bisect"
            elif name.startswith("r") and t["status"] == "bisect":
                mid = interp(t["stratum"], t["pass"], t["fail"], 0.5)
                f = fails(base, seeds)
                if f is None:
                    continue
                t["history"].append({"round": name, "value": mid, "fails": f})
                if f:
                    t["fail"] = mid
                else:
                    t["pass"] = mid
            elif name == "min":
                t.setdefault("necessary_conditions", [])
                for tag, _ in SECONDARY_RESETS:
                    r = results.get(f"adaptive-bs-min-t{t['id']:05d}-{tag}-s0")
                    if r is not None and r["success"]:
                        t["necessary_conditions"].append(tag)
        st["spent"] += len(results)
        st["rounds_done"].append({"round": name, "cases": len(results)})
        if name == "r00":
            st["phase"], st["round"] = "bisect", 1
        elif name.startswith("r") and st["phase"] == "bisect":
            st["round"] += 1
            if st["round"] > ACFG["boundary_search"]["rounds"]:
                for t in st["tracks"]:
                    if t["status"] == "bisect":
                        t["status"] = "bounded"
                st["phase"] = "minimize"
        elif name == "min":
            st["phase"] = "fill"
        elif name == "fill":
            st["phase"] = "done"
        self.save()

    def counterexamples(self, limit: int = 200) -> list[dict]:
        ranked = sorted([t for t in self.state["tracks"] if t["status"] in ("bounded", "fails_at_easy")], key=lambda t: self.difficulty(t))
        out = []
        for t in ranked[:limit]:
            out.append({"track": t["id"], "stratum": t["stratum"], "profile": t["profile"], "base_case": t["base_case"], "status": t["status"],
                        "factor": STRATA[t["stratum"]]["factor"], "last_pass": t["pass"], "first_fail": t["fail"],
                        "normalized_boundary": round(self.difficulty(t), 4), "necessary_conditions": t.get("necessary_conditions", []),
                        "repro": f"./ckt repro adaptive-bs-fill-t{t['id']:05d}-p0-s0  (or any round's case for this track)"})
        return out
