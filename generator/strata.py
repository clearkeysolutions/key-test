"""Case construction for every stratum (SPEC §4).

A case = symbol parameters + base conditions (clean for sweeps, realistic for
joint cases) + the stratum's primary factor at a severity value + physical
placement. `make_case` is a pure function of (cohort master seed, stratum,
profile, bin, mode, index, severity override), which is what lets the
adaptive boundary search re-run a case with only its primary factor changed.

Every stratum's factor is an `apply_<factor>` function that edits the
parameter dict and the geometry context; cross-condition cases simply apply
two of them in sequence.
"""
from __future__ import annotations

import math
import random

from generator.common import derive_seed, load_yaml, log_uniform, rng_for
from generator.geometry.devices import (DEVICES, device_cycle, device_params, frame_of, place, rotation_extent)
from generator.negative_cases.negatives import negative_fields
from generator.payloads.payloads import PATTERNS, capacity_of, symbol_params

CFG = load_yaml("strata.yaml")
STRATA: dict = CFG["strata"]
DECLARED_MIN = CFG["declared_min_ppm"]
GEOM = CFG["module_geometry"]

DARK = [20, 20, 20]
LIGHT = [235, 235, 235]


class Geo:
    """Placement state accumulated while building a case."""

    def __init__(self, sym_geo: dict):
        self.modules = sym_geo["modules"]
        self.cpm = sym_geo["canvas_per_module"]
        self.ppm = None           # target px per module in the OUTPUT frame
        self.module_mm = None
        self.tilt = 0.0           # combined tilt magnitude (deg) for fit estimates
        self.keep = "none"        # protected quantity: "ppm" (ppm is the factor), "roll", or "none"
        self.crop = 0.0           # S16: deliberate fraction of the symbol outside the frame
        self.keep_module = False  # S13: the physical module (screen px per module) is the factor


# ---------------------------------------------------------------- base conditions
def base_clean(p: dict, g: Geo, r: random.Random, k: int) -> None:
    polarity = "dark_on_light" if k % 2 == 0 else "light_on_dark"
    p.update({"fg_rgb": DARK if polarity == "dark_on_light" else LIGHT, "bg_rgb": LIGHT if polarity == "dark_on_light" else [18, 18, 18]})
    p["sheet_rgb"] = [240, 240, 240] if (k // 2) % 2 == 0 else [40, 40, 40]
    p.update({"substrate": "ideal", "roll": round(r.uniform(-8, 8), 3), "yaw": round(r.gauss(0, 3), 3), "pitch": round(r.gauss(0, 3), 3),
              "center_x": round(r.uniform(0.45, 0.55), 4), "center_y": round(r.uniform(0.45, 0.55), 4),
              "illum": round(r.uniform(1.0, 1.5), 4), "clutter": 0.15})
    g.module_mm = log_uniform(r, 0.3, 1.2)
    g.ppm = DECLARED_MIN[p["profile"]] * r.uniform(1.3, 2.2)
    g.tilt = math.hypot(p["yaw"], p["pitch"])


def base_joint(p: dict, g: Geo, r: random.Random, k: int, scale: float = 1.0) -> None:
    """Randomized realistic background conditions; `scale` (S20) multiplies severities."""
    base_clean(p, g, r, k)
    s = scale
    p.update({"roll": round(r.uniform(0, 360), 3), "yaw": round(max(-35, min(35, r.gauss(0, 12 * s))), 3), "pitch": round(max(-35, min(35, r.gauss(0, 12 * s))), 3),
              "center_x": round(r.uniform(0.3, 0.7), 4), "center_y": round(r.uniform(0.3, 0.7), 4),
              "illum": round(log_uniform(r, 0.15 / s, 2.0), 4), "clutter": round(r.uniform(0, 0.8), 3)})
    if r.random() < 0.1 * s:
        p["distractors"] = r.randint(1, 3)
    p["defocus_gauss_px"] = round(abs(r.gauss(0, 0.4 * s)), 4)
    p["motion_px"] = round(abs(r.gauss(0, 1.0 * s)), 4)
    p["motion_angle"] = round(r.uniform(0, 360), 2)
    if r.random() < 0.3:
        p["rs_ratio"] = round(r.uniform(0, 2), 3)
    if r.random() < 0.15 * s:
        p.update({"glare_spots": 1, "glare_peak": round(r.uniform(1.5, 4), 3), "glare_radius_frac": round(r.uniform(0.05, 0.15), 4)})
    if r.random() < 0.2 * s:
        p.update({"shadows": r.randint(1, 2), "shadow_depth": round(r.uniform(0.2, 0.6), 3), "shadow_soft_mm": round(r.uniform(0.3, 4), 3)})
    if r.random() < 0.25:
        p["jpeg_q"] = r.randint(60, 95)
    if r.random() < 0.25:
        p.update({"color_mode": "rgb", "bayer": 1})
    u = r.random()
    if u < 0.45:
        p.update({"substrate": "print", "print_dpi": (300, 600, 1200)[k % 3], "dot_gain_mm": round(r.uniform(0, 0.06), 4)})
    elif u < 0.60:
        p.update({"substrate": "display", "disp_ppi": (326, 401, 460)[k % 3]})
    if r.random() < 0.3:
        p["tone"] = 1
    if r.random() < 0.2:
        p["video_range"] = 1
    if r.random() < 0.1 and p["frame_w"] >= 2160:
        p["rescale"] = 0.5
    g.module_mm = log_uniform(r, 0.25, 1.5)
    g.ppm = DECLARED_MIN[p["profile"]] * log_uniform(r, 0.9, 3.0)
    g.tilt = math.hypot(p["yaw"], p["pitch"])


# ---------------------------------------------------------------- factors
def sample_in_bin(stratum: str, b: int, r: random.Random) -> float:
    lo, hi = STRATA[stratum]["bins"][b]
    lo, hi = min(lo, hi), max(lo, hi)
    if stratum in ("S03", "S08"):
        return log_uniform(r, lo, hi)
    return r.uniform(lo, hi)


def feasible_max_ppm(p: dict, g: Geo, extent: float) -> float:
    w, h = frame_of("phone_4k_wide")
    return 0.9 * min(w, h) / (g.cpm * extent * (1 + 0.6 * math.sin(math.radians(min(80, g.tilt)))))


def apply_S01(p, g, r, x, mode):
    # Clean references. Sweep: mathematically perfect raster (no camera); joint: clean physical capture.
    lo = DECLARED_MIN[p["profile"]]
    if mode in ("sweep",):
        p.update({"ideal": 1, "roll": float(90 * r.randint(0, 3)), "yaw": 0.0, "pitch": 0.0})
        g.tilt = 0.0
        extent = 1.0
    else:
        extent = 1.15
    hi = max(lo * 1.05, min(16.0, feasible_max_ppm(p, g, extent)))
    g.ppm = math.exp(math.log(lo) + x * (math.log(hi) - math.log(lo)))
    if p.get("ideal"):
        p["ideal_ppm"] = round(g.ppm, 4)
    g.keep = "ppm"


def apply_S02(p, g, r, x, mode):
    pass  # handled in symbol construction (fill = x, pattern cycled)


def apply_S03(p, g, r, x, mode):
    g.ppm = x
    g.keep = "ppm"
    # Subpixel phase: centre at arbitrary sub-pixel offsets; downsampling in 30% (4K -> preview path).
    p["center_x"] = round(r.uniform(0.4, 0.6) + r.random() / p["frame_w"], 6)
    p["center_y"] = round(r.uniform(0.4, 0.6) + r.random() / p["frame_h"], 6)
    if r.random() < 0.3 and p["frame_w"] >= 2160:
        f = r.choice([0.5, 0.6667, 0.75])
        if x / f * g.cpm * 1.1 <= 0.9 * 2160:  # the pre-scale symbol must still fit the sensor frame
            p["rescale"] = f


def apply_S04(p, g, r, x, mode):
    a = r.uniform(0, 2 * math.pi)
    p["yaw"], p["pitch"] = round(x * math.cos(a), 3), round(x * math.sin(a), 3)
    g.tilt = x
    if r.random() < 0.3:  # nonplanar: cylinder (bottles, curled labels)
        p["bend_radius_mm"] = round(r.uniform(0.8, 4.0) * (g.module_mm or 1) * g.cpm * r.choice([1, -1]), 3)
        p["bend_axis"] = round(r.uniform(0, 180), 2)


def apply_S05(p, g, r, x, mode):
    if r.random() < 0.2:  # finder orientation exactly at quarter turns
        x = 90 * round(x / 90) + r.uniform(-0.5, 0.5)
    p["roll"] = round(x % 360, 4)
    g.keep = "roll"


def apply_S06(p, g, r, x, mode):
    if r.random() < 0.5:
        p["defocus_gauss_px"] = None  # filled after placement (needs final ppm)
        p["_blur_modules"] = ("gauss", x)
    else:
        p["_blur_modules"] = ("disc", x)


def apply_S07(p, g, r, x, mode):
    p["_motion_modules"] = x
    p["motion_angle"] = round(r.uniform(0, 360), 2)
    if r.random() < 0.5:
        p["rs_ratio"] = round(r.uniform(0, 3), 3)
    if r.random() < 0.2:
        p["_rot_motion"] = True
    if r.random() < 0.1:
        p["zoom_blur"] = round(r.uniform(0, 0.03), 4)


def apply_S08(p, g, r, x, mode):
    dev = DEVICES[p["device"]]
    p["illum"] = round(x / dev["max_signal_e"], 6)
    if x < 500:
        p["read_noise_e"] = round(dev["read_noise_e"] * r.uniform(1.0, 2.5), 3)
    if r.random() < 0.5:
        p["denoise"] = round(r.uniform(0.3, 0.9), 3)
    if r.random() < 0.3:
        p["jpeg_q"] = r.randint(70, 90)


def apply_S09(p, g, r, x, mode):
    # Grey levels with Michelson contrast x around a random mean reflectance.
    from generator.colour import grey_pair_for_contrast
    dark, light = grey_pair_for_contrast(x, r)
    if p["fg_rgb"][0] < 128:
        p["fg_rgb"], p["bg_rgb"] = dark, light
    else:
        p["fg_rgb"], p["bg_rgb"] = light, dark
    if r.random() < 0.6:
        p.update({"grad": round(r.uniform(0, 0.8), 3), "grad_angle": round(r.uniform(0, 360), 2)})
    if r.random() < 0.6:
        p.update({"shadows": r.randint(1, 3), "shadow_depth": round(r.uniform(0.3, 0.85), 3), "shadow_soft_mm": round(r.uniform(0.2, 5), 3)})


def apply_S10(p, g, r, x, mode):
    n = r.randint(1, 3)
    p["glare_spots"] = n
    p["glare_peak"] = round(r.uniform(4, 12), 3)
    # Saturated area ~ n * pi * (1.2 r)^2 of the canvas area: solve r as a fraction of the canvas.
    p["glare_radius_frac"] = round(math.sqrt(max(1e-6, x) / (n * math.pi)) / 1.2, 5)
    if r.random() < 0.4:
        p["veil"] = round(r.uniform(0, 0.3), 4)
    if r.random() < 0.3:
        p.update({"substrate": "print", "print_dpi": 600})


OCCLUSION_KINDS = ("scratch", "tear", "missing_corner", "sticker", "finder_cover", "smudge", "finger")


def apply_S11(p, g, r, x, mode, k=0):
    p["occl_kind"] = OCCLUSION_KINDS[k % len(OCCLUSION_KINDS)]
    p["occl_frac"] = round(x, 5)
    p["occl_count"] = r.randint(1, 4) if p["occl_kind"] in ("scratch", "sticker", "smudge") else 1


def apply_S12(p, g, r, x, mode, k=0):
    p["substrate"] = "print"
    p["print_dpi"] = (300, 600, 1200, 2400)[k % 4]
    p["_dot_gain_modules"] = -x if r.random() < 0.15 else x
    p["paper_blur_mm"] = round(r.uniform(0.005, 0.04), 4)
    p["paper_texture"] = round(r.uniform(0.01, 0.06), 4)
    paper = r.choice(["matte", "glossy", "newsprint"])
    if paper == "newsprint":
        p["bg_rgb" if p["fg_rgb"][0] < 128 else "fg_rgb"] = [200, 196, 185]
    if paper == "glossy" and r.random() < 0.5:
        p.update({"glare_spots": 1, "glare_peak": round(r.uniform(1.5, 3), 3), "glare_radius_frac": round(r.uniform(0.05, 0.12), 4)})
    if r.random() < 0.25:
        p.update({"halftone_lpi": r.choice([85, 100, 133, 150]), "ink_density": round(r.uniform(0.6, 0.95), 3)})


def apply_S13(p, g, r, x, mode, k=0):
    ppi = (120, 220, 326, 401, 460)[k % 5]
    p.update({"substrate": "display", "disp_ppi": ppi, "color_mode": "rgb", "bayer": 1,
              "disp_fill": round(r.uniform(0.5, 0.9), 3), "disp_layout": r.choice([0, 0, 1, 2]),
              "disp_brightness": round(r.uniform(0.8, 3.0), 3), "disp_glass": round(r.uniform(0.02, 0.08), 4)})
    g.module_mm = x * 25.4 / ppi     # x display pixels per module
    g.keep_module = True
    if r.random() < 0.4:
        p.update({"disp_refresh_amp": round(r.uniform(0.05, 0.3), 3), "disp_refresh_period": round(r.uniform(100, 600), 1), "rs_ratio": round(r.uniform(0.5, 3), 3)})
    if r.random() < 0.2:
        p.update({"glare_spots": 1, "glare_peak": round(r.uniform(1, 3), 3), "glare_radius_frac": 0.1})
    g.ppm = r.uniform(3.0, 6.0)      # camera px per module: the moire-prone range


def apply_S14(p, g, r, x, mode):
    p["jpeg_q"] = int(round(x))
    p["sharpen"] = round(r.uniform(0, 1.5), 3)
    p["denoise"] = round(r.uniform(0, 1.0), 3)
    if r.random() < 0.5:
        p["tone"] = 1
    if r.random() < 0.3:
        p["video_range"] = 1
    if r.random() < 0.3 and p["frame_w"] >= 2160:
        p["rescale"] = r.choice([0.5, 0.75])


def apply_S15(p, g, r, x, mode):
    from generator.colour import colour_pair_for_ratio
    fg, bg = colour_pair_for_ratio(x, r)
    p.update({"color_mode": "rgb", "bayer": 1, "fg_rgb": fg, "bg_rgb": bg})
    if r.random() < 0.2:
        p["bg_alpha"] = round(r.uniform(0.0, 0.6), 3)
        p["sheet_rgb"] = [r.randint(60, 220) for _ in range(3)]
    if r.random() < 0.3:
        p["mix_srgb"] = 0
    p["ca_px"] = round(r.uniform(0, 2), 3)


def apply_S16(p, g, r, x, mode):
    cls = int(x)
    if cls == 0:
        p["clutter"] = round(r.uniform(0.3, 0.6), 3)
    elif cls == 1:
        p.update({"clutter": round(r.uniform(0.6, 1.0), 3), "distractors": r.randint(2, 5)})
    elif cls == 2 or cls == 3:  # class 3 negatives cannot carry a second valid code: distractors instead
        p.update({"clutter": round(r.uniform(0.3, 0.8), 3), "distractors": r.randint(6, 12)})
    elif cls == 3 and p.get("neg_kind", "none") == "none":
        p.update({"second_code": 1, "expected": "DECODE_ANY_OF", "clutter": round(r.uniform(0.2, 0.6), 3)})
        g.ppm = min(g.ppm, DECLARED_MIN[p["profile"]] * 1.6)
    else:
        g.crop = r.uniform(0.03, 0.25)
    p["severity_detail"] = None


def apply_S17(p, g, r, x, mode):
    prof = p["profile"]
    slots = GEOM["ring_slots"].get(prof) or 72 * g.modules / 29
    p["rot_blur_deg"] = round(x * 360.0 / slots, 5)
    if r.random() < 0.3:
        p.update({"occl_kind": "scratch", "occl_frac": round(r.uniform(0.005, 0.03), 4), "occl_count": 1})
    g.ppm = DECLARED_MIN[prof] * r.uniform(1.0, 1.6)


def apply_S18(p, g, r, x, mode):
    prof = p["profile"]
    key = prof if prof != "P0" else ("P0_print" if p.get("p0_layout") == "print" else "P0_standard")
    pitch = GEOM["ray_pitch_modules"][key]
    g.ppm = x / pitch
    g.keep = "ppm"
    if r.random() < 0.3:
        p["zoom_blur"] = round(r.uniform(0.005, 0.03), 4)
    p["roll"] = round(r.choice([0, 45, 90, 135, 180, 225, 270, 315]) + r.uniform(-3, 3), 3)


def apply_S19(p, g, r, x, mode):
    cls = int(x)
    if cls in (0, 4):
        p["k1"] = round(r.uniform(-0.05, 0.05), 4)
    if cls in (1, 4):
        p.update({"device": "phone_4k_ultrawide", **{k: v for k, v in device_params("phone_4k_ultrawide").items() if k not in ("sharpen", "denoise")}})
        p["k1"] = round(r.uniform(-0.15, -0.08), 4)
        p["center_x"] = round(r.choice([0.22, 0.78]), 3)
        p["center_y"] = round(r.choice([0.18, 0.82]), 3)
    if cls in (2, 3, 4):
        p.update({"occl_kind": "finder_cover", "occl_frac": round(r.uniform(0.01, 0.03) if cls == 2 else r.uniform(0.03, 0.06), 4)})
    if cls == 3:
        p.update({"glare_spots": 1, "glare_peak": round(r.uniform(3, 8), 3), "glare_radius_frac": 0.08})
    if cls == 4:
        t = r.uniform(30, 45); a = r.uniform(0, 2 * math.pi)
        p["yaw"], p["pitch"] = round(t * math.cos(a), 3), round(t * math.sin(a), 3)
        g.tilt = t


def apply_S20(p, g, r, x, mode, k=0):
    # Joint realistic conditions at severity multiplier x; half are temporal sequences.
    if k % 2 == 0:
        p.update({"frames": r.randint(3, 10), "shake_px": round(r.uniform(2, 15) * x, 3), "shake_roll": round(r.uniform(0.5, 3) * x, 3),
                  "frame_motion_px": round(r.uniform(0.5, 4) * x, 3), "focus_drift": round(r.uniform(0.1, 0.5) * x, 3),
                  "exposure_drift": round(r.uniform(0.05, 0.25) * x, 3)})


FACTORS = {s: globals()[f"apply_{s}"] for s in STRATA}
NEEDS_K = {"S11", "S12", "S13", "S20"}


# ---------------------------------------------------------------- placement & feasibility
def ensure_fit(p: dict, g: Geo) -> bool:
    """Make the symbol fit the frame (unless a crop is intended) without changing the protected quantity."""
    need = g.ppm * g.cpm * (1 + 0.6 * math.sin(math.radians(min(80, g.tilt)))) / p.get("rescale", 1.0)
    candidates = [p["device"]] + [d for d in ("phone_4k_wide",) if d != p["device"]]
    for dev in candidates:
        w, h = frame_of(dev)
        max_extent = 0.9 * min(w, h) / need
        ext = rotation_extent(p["roll"])
        if max_extent >= ext:
            pass
        elif max_extent >= 1.0 and g.keep == "ppm":
            # Reduce the in-plane angle within its quadrant until it fits.
            quad = math.floor(p["roll"] / 90.0) * 90.0
            a_max = math.degrees(math.asin(min(1.0, max_extent / math.sqrt(2)))) - 45.0
            frac = p["roll"] - quad
            p["roll"] = round(quad + (min(frac, a_max) if frac <= 45 else max(frac, 90 - a_max)), 4)
        else:
            continue
        if dev != p["device"]:
            keep = {k: p[k] for k in ("sharpen", "denoise") if k in p}
            p.update(device_params(dev))
            p.update(keep)
        return True
    return False


def finalize_geometry(p: dict, g: Geo, r: random.Random) -> list[str]:
    notes = []
    if not ensure_fit(p, g):
        # Protected quantity cannot fit any device: shrink ppm to the 4K limit and record it.
        w, h = frame_of("phone_4k_wide")
        p.update(device_params("phone_4k_wide"))
        g.ppm = 0.9 * min(w, h) / (g.cpm * rotation_extent(p["roll"]) * (1 + 0.6 * math.sin(math.radians(min(80, g.tilt))))) * p.get("rescale", 1.0)
        if g.keep == "ppm":  # only a design deviation when ppm is the stratum's own factor
            notes.append("ppm_capped_to_fit")
    pre_scale_ppm = g.ppm / p.get("rescale", 1.0)
    pre_scale_ppm = place(p, g.module_mm, pre_scale_ppm, g.cpm, keep_module=g.keep_module)
    g.ppm = pre_scale_ppm * p.get("rescale", 1.0)
    if g.crop > 0:
        # Move the symbol so `crop` of its width leaves the frame across one edge.
        side_px = pre_scale_ppm * g.cpm
        edge = r.choice(["l", "r", "t", "b"])
        if edge in "lr":
            c = (side_px * (0.5 - g.crop)) / p["frame_w"]
            p["center_x"] = round(c if edge == "l" else 1 - c, 5)
        else:
            c = (side_px * (0.5 - g.crop)) / p["frame_h"]
            p["center_y"] = round(c if edge == "t" else 1 - c, 5)
    # Factors expressed relative to the module pitch need the final ppm.
    if "_blur_modules" in p:
        kind, x = p.pop("_blur_modules")
        if kind == "gauss":
            p["defocus_gauss_px"] = round(x * pre_scale_ppm, 4)
        else:
            p["defocus_disc_px"] = round(2 * x * pre_scale_ppm, 4)  # variance-equivalent sigma = R/2
    if "_motion_modules" in p:
        p["motion_px"] = round(p.pop("_motion_modules") * pre_scale_ppm, 4)
        if p.pop("_rot_motion", False):
            p["rot_blur_deg"] = round(math.degrees(p["motion_px"] / max(1.0, 0.5 * pre_scale_ppm * g.cpm)), 5)
            p["motion_px"] = 0.0
    if "_dot_gain_modules" in p:
        module_mm = p["symbol_mm"] / g.cpm
        p["dot_gain_mm"] = round(p.pop("_dot_gain_modules") * module_mm, 5)
    p.pop("severity_detail", None)
    return notes


# ---------------------------------------------------------------- the case
def make_case(cohort: str, master: str, stratum: str, profile: str, b: int, mode: str, k: int,
              severity: float | None = None, case_id: str | None = None, extra_factors: list | None = None,
              family_size: int = 5, seed_labels: tuple = ()) -> dict:
    """Builds one case. `extra_factors` = [(stratum, severity), ...] are applied after the primary (cross-condition)."""
    label = (cohort, stratum, profile, b, mode, k) + tuple(seed_labels)
    r = rng_for(master, "params", *label)
    rs = rng_for(master, "severity", *label)
    x = severity if severity is not None else sample_in_bin(stratum, b, rs)
    fill = x if stratum == "S02" else None
    pattern = PATTERNS[k % len(PATTERNS)] if stratum == "S02" else None
    sym, sgeo = symbol_params(profile, k + 7 * b, r, fill=fill, pattern=pattern)
    if severity is None and stratum in ("S03", "S18"):
        # Keep the sampled severity inside the physically feasible part of its bin
        # (the largest symbols cannot be imaged at high px/module in any phone frame).
        lo, hi = sorted(STRATA[stratum]["bins"][b])
        ppm_fit = 0.9 * 2160 / (sgeo["canvas_per_module"] * 1.06)
        pitch = 1.0
        if stratum == "S18":
            key = profile if profile != "P0" else ("P0_print" if sym.get("p0_layout") == "print" else "P0_standard")
            pitch = GEOM["ray_pitch_modules"][key]
        cap = ppm_fit * pitch
        if x > cap and lo < cap:
            x = lo + (x - lo) / (hi - lo) * (cap - lo)
    p: dict = {"cohort": cohort, "stratum": stratum, "sev_bin": b, "mode": mode, "factor": STRATA[stratum]["factor"], "severity": round(float(x), 6)}
    p.update(sym)
    device = device_cycle(k, offset=derive_seed(master, "devoff", stratum, profile, b) % 100)
    p.update(device_params(device))
    g = Geo(sgeo)
    negative = mode == "negative"
    realistic = mode in ("joint",) or (negative and k % 2 == 1) or mode.startswith("adaptive") or mode == "holdout"
    if stratum == "S20":
        base_joint(p, g, r, k, scale=x)
    elif realistic and stratum != "S01":
        base_joint(p, g, r, k)
    else:
        base_clean(p, g, r, k)
    p["expected"] = "DECODE"
    if negative:
        p.update(negative_fields(profile, stratum, k, r))
    fn = FACTORS[stratum]
    (fn(p, g, r, x, mode, k) if stratum in NEEDS_K else fn(p, g, r, x, mode))
    for (s2, x2) in (extra_factors or []):
        f2 = FACTORS[s2]
        (f2(p, g, r, x2, "joint", k) if s2 in NEEDS_K else f2(p, g, r, x2, "joint"))
    notes = finalize_geometry(p, g, r)
    fam = k // family_size
    p["payload_family"] = f"{cohort}/{stratum}/{profile}/{b}/{mode}/{fam}"
    p["payload_seed"] = str(derive_seed(master, "payload", cohort, stratum, profile, b, mode, fam, *seed_labels))
    p["seed"] = str(derive_seed(master, "case", *label))
    p["device_seed"] = str(derive_seed("device-units", p["device"], "holdout" if cohort == "holdout" else "campaign"))
    if p.get("expected") == "DECODE_ANY_OF" and negative:
        p["expected"] = "NO_WRONG"
    p["case_id"] = case_id or f"{cohort}-{stratum}-{profile}-b{b}-{mode[:3]}-{k:05d}"
    if notes:
        p["mode"] = p["mode"]  # notes are reported by plan validation, not stored in the case
        p["_notes"] = notes
    # Clean output: drop None values; the worker supplies defaults for absent keys.
    return {k2: v for k2, v in p.items() if v is not None}


def payload_capacity(case: dict) -> int:
    return capacity_of(case)
