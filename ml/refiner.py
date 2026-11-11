"""CKey ML corner refiner (training side).

Input: a Profile 0 symbol rectified through rough corners over canonical [-3, 32]^2 at
128 x 128 (ckey_ml_rectify_region_gray8), standardised (ml_standardise in the core).
Output: 8 values, each canonical corner's correction in canonical units (true corner in
the rough corners' frame minus the canonical corner). The core applies it twice
(ml_sites.inc ml_refine_quad) and retries a failed decode from the refined corners.
Records: `ckt_worker --ml-refine` (fresh seeds; true corners plus 0.2-1.5 module noise,
any corner order and winding).

  python -m ml.refiner train DATA_DIR OUT_DIR [--epochs N] [--width W]
  python -m ml.refiner export OUT_DIR/best.pt OUT.ckml
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import struct
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

from ml.locator import ConvBN, GraphWriter, fold, pick_device

SIDE = 128
REC = np.dtype([("magic", "<u4"), ("ver", "<u4"), ("hash", "u1", 16), ("stratum", "u1"), ("v", "u1"), ("layout", "u1"), ("gtq", "u1"),
                ("sigma", "<f4"), ("t", "<f4", 8), ("px", "u1", (SIDE, SIDE))])
assert REC.itemsize == 16448
GRID = [29, 31, 33, 36, 38, 41, 44, 47, 50, 54, 58, 62, 67, 71, 77, 82, 88, 94, 101, 108, 116, 124, 133, 143, 153, 164, 176, 188, 202, 216, 232]


def load(data: Path):
    parts = [np.memmap(f, dtype=REC, mode="r") for f in sorted(glob.glob(str(data / "*.bin"))) if Path(f).stat().st_size >= REC.itemsize]
    r = np.concatenate(parts) if parts else np.zeros(0, REC)
    if len(r) and not (r["magic"] == 0x524D4B43).all():
        raise SystemExit("corrupt refiner record")
    return r


def standardise(px):
    x = px.astype(np.float32)
    return (x - x.mean()) / max(4.0, float(x.std()))


class Records(torch.utils.data.Dataset):
    def __init__(self, r, train):
        self.r, self.train = r, train

    def __len__(self):
        return len(self.r)

    def __getitem__(self, i):
        rec = self.r[i]
        x = rec["px"].astype(np.float32)
        if self.train:
            rng = np.random.default_rng(i * 7919 + (int(time.time() * 1e6) & 0xFFFFFF))
            x = np.clip((x / 255.0) ** float(np.exp(rng.normal(0, 0.3))) * rng.uniform(0.6, 1.2) + rng.uniform(-0.15, 0.15), 0, 1) * 255
            x = x + rng.normal(0, rng.uniform(0, 8), x.shape)
        t = np.clip(rec["t"], -3, 3).astype(np.float32)
        return torch.from_numpy(standardise(x)[None]), torch.from_numpy(t), torch.tensor(float(GRID[int(rec["v"]) - 1]))


class Block(nn.Module):
    def __init__(self, c):
        super().__init__()
        self.body = nn.Sequential(ConvBN(c, c, 3, groups=c), ConvBN(c, c, 1, act=False))

    def forward(self, x):
        return x + self.body(x)


class Refiner(nn.Module):
    """128 -> 1 by seven stride-2 stages (all convolutions), then a linear 1x1 head."""
    def __init__(self, width: float = 1.0):
        super().__init__()
        self.width = width
        C = [max(8, int(round(c * width / 8)) * 8) for c in (16, 32, 48, 64, 96, 128, 128)]
        layers = [ConvBN(1, C[0], 3, 2)]
        for a, b, blocks in zip(C[:-1], C[1:], (1, 1, 1, 0, 0, 0)):
            layers += [ConvBN(a, a, 3, 2, groups=a), ConvBN(a, b, 1)] + [Block(b) for _ in range(blocks)]
        self.body = nn.Sequential(*layers)
        self.head = nn.Conv2d(C[-1], 8, 1)
        with torch.no_grad():
            self.head.weight.mul_(0.1); self.head.bias.zero_()

    def forward(self, x):
        return self.head(self.body(x))


def evaluate(model, r, idx, dev):
    model.eval(); errs = []
    with torch.no_grad():
        for s in range(0, len(idx), 64):
            ii = idx[s:s + 64]
            x = torch.stack([torch.from_numpy(standardise(r[i]["px"])[None]) for i in ii]).to(dev)
            out = model(x)[:, :, 0, 0].cpu().numpy()
            for j, i in enumerate(ii):
                t = r[i]["t"]; m = GRID[int(r[i]["v"]) - 1] / 29.0     # modules per canonical unit
                before = np.hypot(t[0::2], t[1::2]).max() * m
                after = np.hypot(t[0::2] - out[j][0::2], t[1::2] - out[j][1::2]).max() * m
                errs.append((before, after))
    e = np.array(errs)
    return {"worst_corner_modules_before_median": float(np.median(e[:, 0])), "after_median": float(np.median(e[:, 1])),
            "after_p90": float(np.percentile(e[:, 1], 90)), "under_0.25_module": float((e[:, 1] < 0.25).mean())}


def train(data, out, epochs, batch, lr, workers, width):
    torch.manual_seed(0)
    dev = pick_device(); out.mkdir(parents=True, exist_ok=True)
    r = load(data)
    h = r["hash"][:, 0].astype(int) * 256 + r["hash"][:, 1]
    va = np.nonzero(h % 20 == 0)[0]; tr = np.nonzero(h % 20 != 0)[0]
    print(f"device {dev}; {len(r)} records: {len(tr)} train, {len(va)} validation", flush=True)
    model = Refiner(width).to(dev)
    print("parameters", sum(p.numel() for p in model.parameters()), flush=True)
    dl = torch.utils.data.DataLoader(Records(r[tr], True), batch_size=batch, shuffle=True, num_workers=workers, drop_last=True, persistent_workers=workers > 0)
    opt = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=lr, total_steps=max(1, epochs * len(dl)), pct_start=0.05)
    log, best = [], None
    for ep in range(epochs):
        model.train(); t0 = time.time(); tot, nb = 0.0, 0
        for x, t, mods in dl:
            x, t, mods = x.to(dev), t.to(dev), mods.to(dev)
            pred = model(x)[:, :, 0, 0]
            # Error in modules, so dense and sparse symbols weigh the same.
            loss = (F.smooth_l1_loss(pred, t, reduction="none", beta=0.02) * (mods / 29.0)[:, None]).mean()
            opt.zero_grad(); loss.backward(); opt.step(); sched.step()
            tot += loss.item(); nb += 1
        ev = evaluate(model, r, va[:3000], dev)
        row = {"epoch": ep + 1, "sec": round(time.time() - t0), "loss": round(tot / max(1, nb), 4), **{k: round(v, 4) for k, v in ev.items()}}
        log.append(row); print(json.dumps(row), flush=True)
        sd = {"width": width, "state": {k: v.cpu() for k, v in model.state_dict().items()}}
        torch.save(sd, out / "model.pt")
        if best is None or ev["after_median"] <= best:
            best = ev["after_median"]; torch.save(sd, out / "best.pt")
    (out / "train_log.json").write_text(json.dumps(log, indent=1))


def export(model_path, out_path):
    ck = torch.load(model_path, map_location="cpu")
    model = Refiner(ck.get("width", 1.0)); model.load_state_dict(ck["state"]); model.eval()
    g = GraphWriter()
    x = 0
    for m in model.body:
        if isinstance(m, Block):
            t = x
            for c in m.body:
                w, b, s_, gr, relu = fold(c); t = g.conv(t, w, b, s_, gr, relu)
            x = g.add(x, t)
        else:
            w, b, s_, gr, relu = fold(m); x = g.conv(x, w, b, s_, gr, relu)
    x = g.conv(x, model.head.weight.detach().float(), model.head.bias.detach().float(), 1, 1, False)
    blob = b"CKML" + struct.pack("<6I", 2, g.nbuf, len(g.ops), SIDE, SIDE, 8) + b"".join(g.ops)
    out_path.write_bytes(blob)
    sha = hashlib.sha256(blob).hexdigest()
    print(f"wrote {out_path}: {len(g.ops)} ops, {g.nbuf} buffers, {len(blob)} bytes, sha256 {sha}")
    return sha


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("train"); t.add_argument("data", type=Path); t.add_argument("out", type=Path)
    t.add_argument("--epochs", type=int, default=25); t.add_argument("--batch", type=int, default=64)
    t.add_argument("--lr", type=float, default=2e-3); t.add_argument("--workers", type=int, default=4); t.add_argument("--width", type=float, default=1.0)
    e = sub.add_parser("export"); e.add_argument("model", type=Path); e.add_argument("out", type=Path)
    a = ap.parse_args()
    if a.cmd == "train":
        train(a.data, a.out, a.epochs, a.batch, a.lr, a.workers, a.width)
    else:
        export(a.model, a.out)


if __name__ == "__main__":
    main()
