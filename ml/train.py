"""Train the corner-localization MLP (numpy only; deterministic).

Input: 24x24 corner patch from a production border candidate, contrast-normalised exactly
as runners/native/ml.h normalise(). Target: the true field corner in the candidate's local
edge frame (side units). Model: 576 -> 128 -> 64 -> 2, ReLU, Adam, Huber loss.
Split: by case hash (all corners of a case on one side) -> no leakage between train/val.
"""
from __future__ import annotations

import hashlib
import json
import struct
import time
from pathlib import Path

import numpy as np

P = 24
REC = np.dtype([("magic", "<u4"), ("version", "<u4"), ("case_hash", "u1", 16), ("cand_idx", "u1"), ("matched", "u1"), ("reader_ok", "u1"),
                ("gtq_ok", "u1"), ("layout", "u1"), ("p0_version", "u1"), ("stratum", "u1"), ("pad", "u1"), ("side_px", "<f4"), ("ppm", "<f4"),
                ("patch", "u1", (4, P, P)), ("label", "<f4", (4, 2)), ("refined", "<f4", (4, 2))])
assert REC.itemsize == 2408


def load(files: list[Path]) -> np.ndarray:
    parts = [np.fromfile(f, dtype=REC) for f in files if f.stat().st_size]
    recs = np.concatenate(parts) if parts else np.zeros(0, dtype=REC)
    if len(recs) and not (recs["magic"] == 0x4C4D4B43).all():
        raise SystemExit("corrupt ML record file")
    return recs


def normalise(patches: np.ndarray) -> np.ndarray:
    x = patches.reshape(len(patches), -1).astype(np.float64)
    mean = x.mean(axis=1, keepdims=True)
    sd = np.sqrt(np.maximum(1.0, (x * x).mean(axis=1, keepdims=True) - mean * mean))
    return ((x - mean) / sd).astype(np.float32)


def corners(recs: np.ndarray):
    """Flatten matched candidates to per-corner samples; returns X, Y, refined, case-split key."""
    m = recs[recs["matched"] == 1]
    X = normalise(m["patch"].reshape(-1, P, P))
    Y = m["label"].reshape(-1, 2)
    R = m["refined"].reshape(-1, 2)
    key = np.repeat(m["case_hash"][:, 0].astype(np.int64) * 256 + m["case_hash"][:, 1], 4)
    ok = np.isfinite(Y).all(axis=1)
    return X[ok], Y[ok], R[ok], key[ok]


class MLP:
    def __init__(self, sizes, seed=7):
        rng = np.random.default_rng(seed)
        self.W = [rng.normal(0, np.sqrt(2 / a), (b, a)).astype(np.float32) for a, b in zip(sizes[:-1], sizes[1:])]
        self.b = [np.zeros(b, np.float32) for b in sizes[1:]]
        self.m = [np.zeros_like(w) for w in self.W + self.b]
        self.v = [np.zeros_like(w) for w in self.W + self.b]
        self.t = 0

    def forward(self, x):
        acts = [x]
        for i, (W, b) in enumerate(zip(self.W, self.b)):
            z = acts[-1] @ W.T + b
            acts.append(np.maximum(z, 0) if i < len(self.W) - 1 else z)
        return acts

    def step(self, x, y, lr=1e-3, delta=1.0):
        acts = self.forward(x)
        err = acts[-1] - y
        g = np.where(np.abs(err) <= delta, err, delta * np.sign(err)) / len(x)  # Huber gradient
        grads_W, grads_b = [], []
        for i in reversed(range(len(self.W))):
            grads_W.insert(0, g.T @ acts[i])
            grads_b.insert(0, g.sum(axis=0))
            if i:
                g = (g @ self.W[i]) * (acts[i] > 0)
        self.t += 1
        params = self.W + self.b
        for j, (p, gr) in enumerate(zip(params, grads_W + grads_b)):
            self.m[j] = 0.9 * self.m[j] + 0.1 * gr
            self.v[j] = 0.999 * self.v[j] + 0.001 * gr * gr
            mh, vh = self.m[j] / (1 - 0.9 ** self.t), self.v[j] / (1 - 0.999 ** self.t)
            p -= (lr * mh / (np.sqrt(vh) + 1e-8)).astype(np.float32)

    def predict(self, x, batch=8192):
        return np.concatenate([self.forward(x[i:i + batch])[-1] for i in range(0, len(x), batch)]) if len(x) else np.zeros((0, 2), np.float32)


def save(model: MLP, scale: float, path: Path) -> str:
    """CKTMLP1 format read by runners/native/ml.h (the target scale is folded into the last layer)."""
    W = [w.copy() for w in model.W]
    b = [x.copy() for x in model.b]
    W[-1] /= scale
    b[-1] /= scale
    buf = bytearray(b"CKTMLP1\0") + struct.pack("<I", len(W))
    for w, bb in zip(W, b):
        buf += struct.pack("<II", w.shape[1], w.shape[0]) + w.astype("<f4").tobytes() + bb.astype("<f4").tobytes()
    path.write_bytes(bytes(buf))
    return hashlib.sha256(bytes(buf)).hexdigest()


def train(record_files: list[Path], out_dir: Path, epochs: int = 15, hidden=(128, 64), seed: int = 7, log=print) -> dict:
    t0 = time.time()
    recs = load(record_files)
    X, Y, R, key = corners(recs)
    if len(X) < 200:
        raise SystemExit(f"only {len(X)} labelled corners: generate more ML cases")
    val = (key * 2654435761 % 10) == 0  # ~10% of cases, all of their corners
    Xt, Yt, Xv, Yv, Rv = X[~val], Y[~val], X[val], Y[val], R[val]
    scale = 10.0  # targets are ~0.0-0.1 side units
    model = MLP([X.shape[1], *hidden, 2], seed)
    rng = np.random.default_rng(seed)
    history = []
    for ep in range(epochs):
        order = rng.permutation(len(Xt))
        lr = 1e-3 * (0.5 if ep >= epochs * 0.6 else 1.0) * (0.5 if ep >= epochs * 0.85 else 1.0)
        for i in range(0, len(order), 1024):
            idx = order[i:i + 1024]
            model.step(Xt[idx], Yt[idx] * scale, lr)
        pv = model.predict(Xv) / scale
        mae = float(np.mean(np.linalg.norm(pv - Yv, axis=1)))
        history.append({"epoch": ep + 1, "val_corner_error_side_units": mae})
        log(f"  epoch {ep + 1}/{epochs}: validation corner error {mae:.5f} side units")
    pv = model.predict(Xv) / scale
    err_ml = np.linalg.norm(pv - Yv, axis=1)
    finite = np.isfinite(Rv).all(axis=1)
    err_ref = np.linalg.norm(Rv[finite] - Yv[finite], axis=1)
    side = np.repeat(recs[recs["matched"] == 1]["side_px"], 4)[np.isfinite(recs[recs["matched"] == 1]["label"].reshape(-1, 2)).all(axis=1)][val]
    out_dir.mkdir(parents=True, exist_ok=True)
    tmp = out_dir / "model.tmp"
    sha = save(model, scale, tmp)
    path = out_dir / f"locator-{sha[:12]}.bin"
    tmp.replace(path)
    data_hash = hashlib.sha256("".join(sorted(hashlib.sha256(f.read_bytes()).hexdigest() for f in record_files)).encode()).hexdigest()
    card = {"model": path.name, "sha256": sha, "architecture": f"MLP {X.shape[1]}-{'-'.join(map(str, hidden))}-2 ReLU, Huber, Adam",
            "parameters": int(sum(w.size + b.size for w, b in zip(model.W, model.b))), "bytes": path.stat().st_size,
            "task": "predict the true Profile 0 field corner from a 24x24 patch around a production border-candidate corner",
            "acceptance": "geometry only; payloads are accepted solely by the production decoder's RS + CRC checks",
            "training_cases_records": int(len(recs)), "train_corners": int(len(Xt)), "val_corners": int(len(Xv)),
            "split": "by case hash (~10% of cases held out with all their corners)",
            "val_corner_error_side_units": {"ml_median": float(np.median(err_ml)), "ml_p95": float(np.percentile(err_ml, 95)),
                                            "classical_refine_median": float(np.median(err_ref)) if len(err_ref) else None,
                                            "classical_refine_p95": float(np.percentile(err_ref, 95)) if len(err_ref) else None},
            "val_corner_error_px_median": float(np.median(err_ml * side)) if len(side) == len(err_ml) else None,
            "history": history, "dataset_sha256": data_hash, "seconds": round(time.time() - t0, 1),
            "data_provenance": "mltrain cohort: own master seed and payload families; disjoint from structured, adaptive and holdout"}
    (out_dir / f"{path.stem}.card.json").write_text(json.dumps(card, indent=1))
    return card
