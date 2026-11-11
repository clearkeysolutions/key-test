"""CKey ML site model (training side): restores a rectified Profile 0 field into an
ink logit map; the core reads each site's logit off the map (ckey ml_sites.inc).

Input: the field rectified at 2 samples per module pitch (ckey_ml_rectify_gray8),
standardised and made ink-positive exactly as ml_sites_input() in the core.
Output: one logit per 2x2 samples (stride 2, one per module cell). Loss: binary cross-entropy at every site's
canonical position, read bilinearly the way the core reads it (ml_map_at).
Ops: conv (groups 1 or depthwise), folded BN, ReLU, add, nearest 2x upsample-add.
Records: `ckt_worker --ml-sites` (fresh seeds; true corners plus corner noise).

  python -m ml.sites train DATA_DIR OUT_DIR [--epochs N] [--width W]
  python -m ml.sites export OUT_DIR/best.pt OUT.ckml
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

TILE = 96          # training crop (samples); must be a multiple of ALIGN
ALIGN = 8          # three stride-2 stages
SITE_DT = np.dtype([("x", "<u2"), ("y", "<u2"), ("k", "u1"), ("p", "u1")])


# ------------------------------------------------------------------------- records
def index_records(files):
    """[(file, offset_px, side, offset_sites, n_sites, meta)] without loading pixels."""
    out = []
    for f in files:
        b = np.memmap(f, dtype=np.uint8, mode="r")
        at, n = 0, len(b)
        while at + 48 <= n:
            magic, ver = struct.unpack_from("<II", b, at)
            if magic != 0x534D4B43 or ver != 1:
                raise SystemExit(f"corrupt site record in {f} at {at}")
            h = bytes(b[at + 8:at + 24])
            st, v, lay, dark, gtq = (int(x) for x in b[at + 24:at + 29])
            ppm, jit, side, ns = struct.unpack_from("<ffII", b, at + 32)
            px = at + 48
            sites = px + side * side
            at = sites + 6 * ns
            out.append((f, px, side, sites, ns, {"hash": h, "stratum": st, "v": v, "layout": lay, "dark": dark, "gtq": gtq, "ppm": ppm, "jitter": jit}))
    return out


_mm = {}


def _map(f):
    if f not in _mm:
        _mm[f] = np.memmap(f, dtype=np.uint8, mode="r")
    return _mm[f]


def load_record(rec):
    f, px, side, sites, ns, meta = rec
    b = _map(f)
    img = np.asarray(b[px:px + side * side]).reshape(side, side)
    s = np.frombuffer(bytes(b[sites:sites + 6 * ns]), SITE_DT)
    return img, s, meta


def standardise(img: np.ndarray, dark: int) -> np.ndarray:
    """Same as ml_sites_input() in the core: whole-field mean/std, ink positive."""
    x = img.astype(np.float32)
    m = float(x.mean())
    sd = max(4.0, float(x.std()))
    return ((-1.0 if dark else 1.0) * (x - m) / sd).astype(np.float32)


def pitch_of(v: int) -> float:
    grid = [29, 31, 33, 36, 38, 41, 44, 47, 50, 54, 58, 62, 67, 71, 77, 82, 88, 94, 101, 108, 116, 124, 133, 143, 153, 164, 176, 188, 202, 216, 232]
    return 29.0 / grid[v - 1]


MAXS = 4096   # max sites per tile (padded)


class Tiles(torch.utils.data.Dataset):
    def __init__(self, recs, train: bool):
        self.r, self.train = recs, train

    def __len__(self):
        return len(self.r)

    def __getitem__(self, i):
        img, sites, meta = load_record(self.r[i])
        rng = np.random.default_rng(i * 7919 + (int(time.time() * 1e6) & 0xFFFFFF if self.train else 0))
        x = standardise(img, meta["dark"])
        side = img.shape[0]
        spacing = pitch_of(meta["v"]) / 2.0
        px = sites["x"].astype(np.float32) / 64.0 / spacing   # in samples (pixel units, 0 = left edge)
        py = sites["y"].astype(np.float32) / 64.0 / spacing
        lab = sites["p"].astype(np.float32)
        kind = sites["k"]
        t = min(TILE, side)
        x0 = int(rng.integers(0, side - t + 1)) if self.train else (side - t) // 2
        y0 = int(rng.integers(0, side - t + 1)) if self.train else (side - t) // 2
        tile = x[y0:y0 + t, x0:x0 + t]
        sx, sy = px - x0, py - y0
        keep = (sx >= 0.5) & (sy >= 0.5) & (sx <= t - 0.5) & (sy <= t - 0.5)
        sx, sy, lab, kind = sx[keep], sy[keep], lab[keep], kind[keep]
        if self.train:
            k = int(rng.integers(0, 4))
            for _ in range(k):   # rotate 90 degrees counter-clockwise: (x, y) -> (y, t - x)
                tile = np.rot90(tile)
                sx, sy = sy, t - sx
            if rng.random() < 0.5:
                tile = tile[:, ::-1]; sx = t - sx
            g = float(np.exp(rng.normal(0, 0.15)))
            tile = tile * g + rng.normal(0, rng.uniform(0, 0.25), tile.shape).astype(np.float32)
        if t < TILE:
            pad = np.zeros((TILE, TILE), np.float32); pad[:t, :t] = tile; tile = pad
        n = min(len(lab), MAXS)
        sel = rng.permutation(len(lab))[:n] if len(lab) > MAXS else np.arange(n)
        S = np.zeros((MAXS, 4), np.float32)
        S[:n, 0], S[:n, 1], S[:n, 2], S[:n, 3] = sx[sel], sy[sel], lab[sel], 1.0
        K = np.zeros(MAXS, np.int64); K[:n] = kind[sel]
        return torch.from_numpy(np.ascontiguousarray(tile)[None]), torch.from_numpy(S), torch.from_numpy(K)


# ------------------------------------------------------------------------- model
class Block(nn.Module):
    def __init__(self, c):
        super().__init__()
        self.body = nn.Sequential(ConvBN(c, c, 3, groups=c), ConvBN(c, c, 1, act=False))

    def forward(self, x):
        return x + self.body(x)


class SiteNet(nn.Module):
    """U-Net-like encoder at full, /2, /4, /8 resolution; output at /2 (one logit per
    module cell) via upsample-add. About 0.9k multiply-adds per input sample."""
    def __init__(self, width: float = 1.0):
        super().__init__()
        self.width = width
        W = [max(8, int(round(c * width / 4)) * 4) for c in (8, 24, 32, 48)]
        blocks = (1, 2, 2)
        self.stem = nn.Sequential(ConvBN(1, W[0], 3))
        self.down = nn.ModuleList()
        for (a, b), nb in zip(zip(W[:-1], W[1:]), blocks):
            self.down.append(nn.Sequential(ConvBN(a, a, 3, 2, groups=a), ConvBN(a, b, 1), *[Block(b) for _ in range(nb)]))
        fpn = W[1]
        self.lat = nn.ModuleList([nn.Identity()] + [ConvBN(c, fpn, 1, act=False) for c in W[1:]])
        self.head = nn.Sequential(ConvBN(fpn, fpn, 3, groups=fpn), ConvBN(fpn, fpn, 1), nn.Conv2d(fpn, 1, 1))

    def forward(self, x):
        f = [self.stem(x)]
        for d in self.down:
            f.append(d(f[-1]))
        y = self.lat[3](f[3])
        for i in (2, 1):
            y = F.interpolate(y, scale_factor=2, mode="nearest") + self.lat[i](f[i])
        return self.head(y)


def sample_logits(out, S, stride: int = 2):
    """Bilinear read at site positions given in input-sample units, matching the core's
    ml_map_at (cell centres at +0.5). Normalised coordinates are those of the input
    field, so the same physical point is read whatever the output stride."""
    B, _, H, W = out.shape
    gx = S[:, :, 0] / (W * stride) * 2 - 1
    gy = S[:, :, 1] / (H * stride) * 2 - 1
    grid = torch.stack([gx, gy], -1)[:, :, None, :]
    return F.grid_sample(out, grid, mode="bilinear", padding_mode="border", align_corners=False)[:, 0, :, 0]


# ------------------------------------------------------------------------- train
def split(recs):
    h = np.array([r[5]["hash"][0] * 256 + r[5]["hash"][1] for r in recs])
    val = (h % 20) == 0
    return [r for r, v in zip(recs, val) if not v], [r for r, v in zip(recs, val) if v]


def evaluate(model, recs, dev, limit=300):
    """Per-site error rate on whole validation fields, by kind, and the share of
    fields whose site errors fit within a nominal 15% (a proxy for RS capacity)."""
    model.eval()
    errs = {0: [0, 0], 1: [0, 0], 2: [0, 0]}
    fits, n = 0, 0
    with torch.no_grad():
        for rec in recs[:limit]:
            img, sites, meta = load_record(rec)
            side = img.shape[0]
            x = torch.from_numpy(standardise(img, meta["dark"]))[None, None].to(dev)
            out = model(x)
            spacing = pitch_of(meta["v"]) / 2.0
            S = torch.zeros(1, len(sites), 4, device=dev)
            S[0, :, 0] = torch.from_numpy(sites["x"].astype(np.float32) / 64 / spacing)
            S[0, :, 1] = torch.from_numpy(sites["y"].astype(np.float32) / 64 / spacing)
            lg = sample_logits(out, S)[0].cpu().numpy()
            wrong = (lg > 0) != (sites["p"] > 0)
            for k in (0, 1, 2):
                m = sites["k"] == k
                errs[k][0] += int(wrong[m].sum()); errs[k][1] += int(m.sum())
            fits += wrong.mean() < 0.15; n += 1
    return {"site_err_grid": errs[0][0] / max(1, errs[0][1]), "site_err_dial": errs[1][0] / max(1, errs[1][1]),
            "site_err_ring": errs[2][0] / max(1, errs[2][1]), "fields_under_15pct": fits / max(1, n)}


def train(data: Path, out: Path, epochs: int, batch: int, lr: float, workers: int, width: float):
    torch.manual_seed(0)
    dev = pick_device()
    out.mkdir(parents=True, exist_ok=True)
    recs = index_records(sorted(glob.glob(str(data / "*.bin"))))
    tr, va = split(recs)
    print(f"device {dev}; {len(recs)} fields: {len(tr)} train, {len(va)} validation", flush=True)
    model = SiteNet(width).to(dev)
    print("parameters", sum(p.numel() for p in model.parameters()), flush=True)
    dl = torch.utils.data.DataLoader(Tiles(tr, True), batch_size=batch, shuffle=True, num_workers=workers, drop_last=True, persistent_workers=workers > 0)
    opt = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=lr, total_steps=max(1, epochs * len(dl)), pct_start=0.05)
    log, best = [], None
    for ep in range(epochs):
        model.train(); t0 = time.time(); tot, nb = 0.0, 0
        for x, S, K in dl:
            x, S = x.to(dev), S.to(dev)
            lg = sample_logits(model(x), S)
            m = S[:, :, 3]
            loss = (F.binary_cross_entropy_with_logits(lg, S[:, :, 2], reduction="none") * m).sum() / m.sum().clamp(min=1)
            opt.zero_grad(); loss.backward(); opt.step(); sched.step()
            tot += loss.item(); nb += 1
        ev = evaluate(model, va, dev)
        row = {"epoch": ep + 1, "sec": round(time.time() - t0), "loss": round(tot / max(1, nb), 4), **{k: round(v, 4) for k, v in ev.items()}}
        log.append(row); print(json.dumps(row), flush=True)
        sd = {"width": width, "state": {k: v.cpu() for k, v in model.state_dict().items()}}
        torch.save(sd, out / "model.pt")
        score = -ev["site_err_grid"] - ev["site_err_ring"] - ev["site_err_dial"]
        if best is None or score >= best:
            best = score; torch.save(sd, out / "best.pt")
    (out / "train_log.json").write_text(json.dumps(log, indent=1))


# ------------------------------------------------------------------------- export
def export(model_path: Path, out_path: Path, nominal_side: int = 64):
    ck = torch.load(model_path, map_location="cpu")
    model = SiteNet(ck.get("width", 1.0)); model.load_state_dict(ck["state"]); model.eval()
    g = GraphWriter()
    def cb(seq, x):
        w, b, s_, gr, relu = fold(seq)
        return g.conv(x, w, b, s_, gr, relu)
    def seq(mods, x):
        for m in mods:
            if isinstance(m, Block):
                t = x
                for c in m.body:
                    t = cb(c, t)
                x = g.add(x, t)
            else:
                x = cb(m, x)
        return x
    f = [seq(model.stem, 0)]
    for d in model.down:
        f.append(seq(d, f[-1]))
    y = cb(model.lat[3], f[3])
    for i in (2, 1):
        y = g.add(y, cb(model.lat[i], f[i]), up=True)
    c = y
    c = cb(model.head[0], c)
    c = cb(model.head[1], c)
    hc = model.head[2]
    c = g.conv(c, hc.weight.detach().float(), hc.bias.detach().float(), 1, 1, False)
    blob = b"CKML" + struct.pack("<6I", 2, g.nbuf, len(g.ops), nominal_side, 2, 1) + b"".join(g.ops)
    out_path.write_bytes(blob)
    sha = hashlib.sha256(blob).hexdigest()
    print(f"wrote {out_path}: {len(g.ops)} ops, {g.nbuf} buffers, {len(blob)} bytes, sha256 {sha}")
    return sha


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("train"); t.add_argument("data", type=Path); t.add_argument("out", type=Path)
    t.add_argument("--epochs", type=int, default=20); t.add_argument("--batch", type=int, default=32)
    t.add_argument("--lr", type=float, default=3e-3); t.add_argument("--workers", type=int, default=4)
    t.add_argument("--width", type=float, default=1.0)
    e = sub.add_parser("export"); e.add_argument("model", type=Path); e.add_argument("out", type=Path)
    a = ap.parse_args()
    if a.cmd == "train":
        train(a.data, a.out, a.epochs, a.batch, a.lr, a.workers, a.width)
    else:
        export(a.model, a.out)


if __name__ == "__main__":
    main()
