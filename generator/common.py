"""Shared configuration, seed derivation and production-library facts.

Seeds (ckt-seed-v1): every case seed is SHA-256 of a '|'-joined label under a
cohort master seed, truncated to 64 bits. Labels never include parameters
that a case could change, so regenerating a plan reproduces it bit-for-bit.
"""
from __future__ import annotations

import hashlib
import json
import math
import os
import random
import subprocess
from functools import lru_cache
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
SEED_VERSION = "ckt-seed-v1"
PROFILES = ("P1", "P2", "P3", "P0")


def load_yaml(name: str) -> dict:
    with open(ROOT / "configs" / name, "r", encoding="utf-8") as fh:
        return yaml.safe_load(fh)


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def derive_seed(master: str, *labels) -> int:
    msg = "|".join([SEED_VERSION, master, *[str(x) for x in labels]]).encode()
    return int.from_bytes(hashlib.sha256(msg).digest()[:8], "big")


def rng_for(master: str, *labels) -> random.Random:
    return random.Random(derive_seed(master, *labels))


def log_uniform(r: random.Random, lo: float, hi: float) -> float:
    return math.exp(r.uniform(math.log(lo), math.log(hi)))


def worker_path() -> Path:
    env = os.environ.get("CKT_WORKER")
    return Path(env) if env else ROOT / "build" / "native" / "ckt_worker"


@lru_cache(maxsize=1)
def capacity_table() -> dict:
    """Capacities from the production library itself (ckt_worker --capacity-table)."""
    cache = ROOT / "campaign" / "state" / "capacity_table.json"
    if cache.exists():
        return json.loads(cache.read_text())
    out = subprocess.run([str(worker_path()), "--capacity-table"], check=True, capture_output=True, text=True).stdout
    return json.loads(out)


@lru_cache(maxsize=1)
def worker_schema() -> dict:
    out = subprocess.run([str(worker_path()), "--print-schema"], check=True, capture_output=True, text=True).stdout
    return json.loads(out)


LEVELS = ("L", "M", "Q", "H")


def p0_capacity(version: int, level: str, layout: str) -> int:
    row = capacity_table()["p0"][version - 1]
    return int(row[layout][LEVELS.index(level)])


def p0_modules(version: int) -> int:
    return int(capacity_table()["p0"][version - 1]["modules"])
