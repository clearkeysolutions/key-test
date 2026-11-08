"""Camera models, pose and module-resolution physics for case generation.

px per module (ppm) at the symbol centre, fronto-parallel:
    ppm = focal_px * module_mm / distance_mm
so the generator chooses a physical module size and a target ppm and solves
for the distance. If that distance is closer than the device's minimum focus
distance, the symbol is scaled up instead (keeping the target ppm), so no case
is silently out of focus unless its stratum asks for defocus.
"""
from __future__ import annotations

import math
import random

from generator.common import load_yaml

_DEV = load_yaml("devices.yaml")
DEVICES: dict = _DEV["devices"]
DEVICE_NAMES = sorted(DEVICES)


def device_cycle(index: int, offset: int = 0) -> str:
    """Deterministic weighted cycle of devices (exact proportions per 100)."""
    slots: list[str] = []
    for name in DEVICE_NAMES:
        slots += [name] * round(DEVICES[name]["weight"] * 100)
    return slots[(index + offset) % len(slots)]


def frame_of(name: str) -> tuple[int, int]:
    w, h = DEVICES[name]["frame"]
    return int(w), int(h)


def max_fit_ppm(device: str, canvas_modules: float, extent: float = 1.0, fill: float = 0.9) -> float:
    w, h = frame_of(device)
    return fill * min(w, h) / (canvas_modules * extent)


def rotation_extent(roll_deg: float) -> float:
    a = math.radians(roll_deg)
    return abs(math.cos(a)) + abs(math.sin(a))


def device_params(name: str) -> dict:
    d = DEVICES[name]
    w, h = frame_of(name)
    return {
        "device": name, "frame_w": w, "frame_h": h, "focal_px": d["focal_px"], "k1": d["k1"], "k2": d["k2"],
        "full_well_e": d["full_well_e"], "read_noise_e": d["read_noise_e"], "max_signal_e": d["max_signal_e"],
        "prnu": d["prnu"], "adc_bits": d["adc_bits"],
        "lens_sigma_px": d["lens_sigma_px"][0], "lens_sigma_edge_px": d["lens_sigma_px"][1],
        "sharpen": d["isp"]["sharpen"], "denoise": d["isp"]["denoise"],
    }


def place(params: dict, module_mm: float, ppm: float, canvas_per_module: float, keep_module: bool = False) -> float:
    """Set symbol_mm and distance_mm for a target ppm; respect the minimum focus
    distance by enlarging the symbol, or (keep_module) by lowering ppm. Returns the ppm used."""
    dev = DEVICES[params["device"]]
    f = dev["focal_px"]
    distance = f * module_mm / ppm
    if distance < dev["min_focus_mm"]:
        if keep_module:
            distance = dev["min_focus_mm"] * 1.05
            ppm = f * module_mm / distance
        else:
            module_mm *= dev["min_focus_mm"] * 1.05 / distance
            distance = f * module_mm / ppm
    params["symbol_mm"] = round(module_mm * canvas_per_module, 4)
    params["distance_mm"] = round(distance, 3)
    params["target_ppm"] = round(ppm, 4)
    return ppm


def thin_lens_coc_px(device: str, focus_mm: float, object_mm: float) -> float:
    """Geometric blur-disc radius (px) for an object at object_mm when focused at focus_mm."""
    d = DEVICES[device]
    fmm, n = d["f_mm"], d["f_number"]
    pitch_mm = fmm / d["focal_px"]
    aperture = fmm / n
    coc = aperture * fmm * abs(object_mm - focus_mm) / (object_mm * max(1e-6, focus_mm - fmm))
    return 0.5 * coc / pitch_mm


def focus_distance_for_coc(device: str, object_mm: float, radius_px: float) -> float:
    """Inverse of thin_lens_coc_px (focus behind the object); recorded for physical traceability."""
    lo, hi = object_mm, object_mm * 50
    for _ in range(60):
        mid = (lo + hi) / 2
        if thin_lens_coc_px(device, mid, object_mm) < radius_px:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2
