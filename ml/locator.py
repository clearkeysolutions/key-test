"""CKey ML locator + quality network (training side).

Model: grayscale 256x256 -> stride-4 64x64 maps:
  ch 0      centre heatmap (one peak per symbol, Gaussian target)
  ch 1..4   corner heatmaps, symbol corner order (corner 0 = canonical field corner 0)
  ch 5..12  centre -> corner offsets (dx, dy per corner, units of 16 input px)
  ch 13..20 corner sub-pixel offsets (dx, dy per corner, units of 1 output cell)
  ch 21     readable-from-true-corners logit (quality gate)
  ch 22     version: (v - 16) / 8
  ch 23     layout logit (print)

Only ops the portable C++ runtime implements: conv (groups 1 or depthwise), folded BN,
ReLU, add, nearest 2x upsample. Data: records from `ckt_worker --ml-frames` (fresh seeds).

  python -m ml.locator train DATA_DIR OUT_DIR [--epochs N]
  python -m ml.locator export OUT_DIR/model.pt OUT.ckml      (portable weight file)
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import math
import struct
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

S = 256          # input side
R = 4            # output stride
O = S // R       # output side
NCH = 24

REC = np.dtype([("magic", "<u4"), ("ver", "<u4"), ("hash", "u1", 16), ("has", "u1"), ("scan_ok", "u1"), ("gtq", "u1"),
                ("layout", "u1"), ("v", "u1"), ("stratum", "u1"), ("neg", "u1"), ("phys", "u1"), ("ppm", "<f4"),
                ("scale", "<f4"), ("ox", "<f4"), ("oy", "<f4"), ("ms", "<f4"), ("fw", "<u2"), ("fh", "<u2"),
                ("quad", "<f4", 8), ("img", "u1", (S, S))])
assert REC.itemsize == 65624


# ------------------------------------------------------------------------- model
class ConvBN(nn.Sequential):
    def __init__(self, cin, cout, k=3, s=1, groups=1, act=True):
        layers = [nn.Conv2d(cin, cout, k, s, k // 2, groups=groups, bias=False), nn.BatchNorm2d(cout)]
        if act:
            layers.append(nn.ReLU(inplace=True))
        super().__init__(*layers)


class Block(nn.Module):
    """Inverted residual (MobileNetV2) without ReLU6: expand 1x1, depthwise 3x3, project 1x1."""
    def __init__(self, cin, cout, s, e=4):
        super().__init__()
        h = cin * e
        self.res = s == 1 and cin == cout
        self.body = nn.Sequential(ConvBN(cin, h, 1), ConvBN(h, h, 3, s, groups=h), ConvBN(h, cout, 1, act=False))

    def forward(self, x):
        y = self.body(x)
        return x + y if self.res else y


WIDTHS = (16, 24, 40, 80, 128)   # strides 2, 4, 8, 16, 32
DEPTHS = (1, 2, 3, 3, 2)
FPN = 48


class Locator(nn.Module):
    def __init__(self, width: float = 1.0):
        super().__init__()
        self.width = width
        W = tuple(max(8, int(round(w * width / 8)) * 8) for w in WIDTHS)
        fpn = max(16, int(round(FPN * width / 8)) * 8)
        self.stem = ConvBN(1, W[0], 3, 2)
        stages, cin = [], W[0]
        for i, (w, d) in enumerate(zip(W, DEPTHS)):
            blocks = []
            for j in range(d):
                blocks.append(Block(cin, w, 2 if (j == 0 and i > 0) else 1, e=1 if i == 0 else 4))
                cin = w
            stages.append(nn.Sequential(*blocks))
        self.stages = nn.ModuleList(stages)
        self.lat = nn.ModuleList([ConvBN(w, fpn, 1, act=False) for w in W[1:]])
        # Depthwise-separable at 64x64: a full 3x3 conv there would cost more than the encoder.
        self.smooth = nn.Sequential(ConvBN(fpn, fpn, 3, groups=fpn), ConvBN(fpn, fpn, 1))
        self.head = nn.Sequential(ConvBN(fpn, fpn, 3, groups=fpn), ConvBN(fpn, fpn, 1), nn.Conv2d(fpn, NCH, 1))
        with torch.no_grad():
            self.head[-1].bias.zero_()
            self.head[-1].bias[0:5] = -4.0   # heatmaps start near zero

    def forward(self, x):
        x = self.stem(x)
        feats = []
        for st in self.stages:
            x = st(x)
            feats.append(x)
        # feats strides: 2, 4, 8, 16, 32 -> top-down to stride 4
        y = self.lat[3](feats[4])
        for i in (2, 1, 0):
            y = F.interpolate(y, scale_factor=2, mode="nearest") + self.lat[i](feats[i + 1])
        y = self.smooth(y)
        return self.head(y)


# ------------------------------------------------------------------------- data
def normalise(img: np.ndarray) -> np.ndarray:
    """Per-image standardisation (the C++ runtime does exactly this)."""
    x = img.astype(np.float32)
    m = x.mean()
    sd = max(4.0, float(x.std()))
    return (x - m) / sd


def gaussian(hm, cx, cy, sigma):
    h, w = hm.shape
    x0, x1 = max(0, int(cx - 3 * sigma)), min(w, int(cx + 3 * sigma) + 2)
    y0, y1 = max(0, int(cy - 3 * sigma)), min(h, int(cy + 3 * sigma) + 2)
    if x0 >= x1 or y0 >= y1:
        return
    ys, xs = np.mgrid[y0:y1, x0:x1]
    g = np.exp(-((xs - cx) ** 2 + (ys - cy) ** 2) / (2 * sigma * sigma))
    hm[y0:y1, x0:x1] = np.maximum(hm[y0:y1, x0:x1], g)


class Frames(torch.utils.data.Dataset):
    def __init__(self, recs, train: bool, seed=0):
        self.r, self.train, self.seed = recs, train, seed

    def __len__(self):
        return len(self.r)

    def __getitem__(self, i):
        rec = self.r[i]
        rng = np.random.default_rng((self.seed * 1000003 + i * 7919 + (int(time.time() * 1e6) if self.train else 0)) & 0xFFFFFFFF)
        q = rec["quad"].reshape(4, 2).astype(np.float64).copy()
        has = bool(rec["has"])
        M = np.array([[1, 0, 0], [0, 1, 0]], np.float64)   # output -> input
        photo = np.array([0, 1, 0, 0, 0, 0, 128], np.float32)  # gamma_log, gain, offset, invert, blur, noise, fill
        if self.train:
            M, q, photo = augment_params(q, has, rng)
        tgt, mask = targets(q, has, rec)
        return torch.from_numpy(np.ascontiguousarray(rec["img"])), torch.from_numpy(M.astype(np.float32)), torch.from_numpy(photo), torch.from_numpy(tgt), torch.from_numpy(mask), torch.tensor(has)


def targets(q, has, rec):
    tgt = np.zeros((NCH, O, O), np.float32)
    mask = np.zeros((O, O), np.float32)
    if not has:
        return tgt, mask
    c = q.mean(axis=0) / R
    if not (0 <= c[0] < O and 0 <= c[1] < O):
        return tgt, mask
    side = np.mean([np.linalg.norm(q[k] - q[(k + 1) % 4]) for k in range(4)]) / R
    ci, cj = int(np.clip(round(c[0]), 0, O - 1)), int(np.clip(round(c[1]), 0, O - 1))
    gaussian(tgt[0], c[0], c[1], max(1.0, side / 10))
    for k in range(4):
        p = q[k] / R
        gaussian(tgt[1 + k], p[0], p[1], 1.2)
        tgt[5 + 2 * k] = (q[k][0] - ci * R) / 16.0
        tgt[6 + 2 * k] = (q[k][1] - cj * R) / 16.0
        if 0 <= p[0] < O and 0 <= p[1] < O:
            pi, pj = int(math.floor(p[0])), int(math.floor(p[1]))
            tgt[13 + 2 * k, pj, pi] = max(1e-3, p[0] - pi)
            tgt[14 + 2 * k, pj, pi] = max(1e-3, p[1] - pj)
    mask[cj, ci] = 1.0
    tgt[21] = float(rec["gtq"])
    tgt[22] = (float(rec["v"]) - 16.0) / 8.0
    tgt[23] = float(rec["layout"])
    return tgt, mask


def augment_params(q, has, rng):
    """Rotation by a multiple of 90 plus a little, zoom (dense codes filling the frame, a
    region of interest), translation. Never a mirror: a mirrored symbol is a negative."""
    ang = rng.integers(0, 4) * (math.pi / 2) + rng.normal(0, 0.15)
    zoom = math.exp(rng.uniform(-0.25, 0.9)) if has else math.exp(rng.uniform(-0.2, 0.6))
    c = q.mean(axis=0) if has else np.array([S / 2, S / 2])
    tx, ty = rng.normal(0, 30, 2)
    ca, sa = math.cos(ang), math.sin(ang)
    A = zoom * np.array([[ca, -sa], [sa, ca]])
    b = np.array([S / 2 + tx, S / 2 + ty]) - A @ c
    Ai = np.linalg.inv(A)
    M = np.hstack([Ai, (-Ai @ b)[:, None]])
    q = (A @ q.T).T + b
    photo = np.array([rng.normal(0, 0.35), rng.uniform(0.5, 1.3), rng.uniform(-0.2, 0.2), float(rng.random() < 0.2),
                      rng.uniform(0.5, 1.8) if rng.random() < 0.3 else 0.0, rng.uniform(0, 0.05), rng.uniform(40, 220)], np.float32)
    return M, q, photo


def apply_batch(img_u8, M, photo):
    """Batched augmentation in torch: affine warp (bilinear, fill), gamma, polarity, gain/offset,
    Gaussian blur, noise; then per-image standardisation. img_u8 (B,S,S) uint8."""
    B = img_u8.shape[0]
    x = img_u8.float()[:, None] / 255.0
    # affine: M maps output pixel -> input pixel; grid_sample wants normalised coords.
    dv = img_u8.device
    ys, xs = torch.meshgrid(torch.arange(S, dtype=torch.float32, device=dv), torch.arange(S, dtype=torch.float32, device=dv), indexing="ij")
    ones = torch.ones_like(xs)
    P = torch.stack([xs, ys, ones], -1).reshape(-1, 3)            # (S*S, 3)
    src = torch.einsum("bij,nj->bni", M, P)                        # (B, S*S, 2)
    grid = (src + 0.5) / S * 2 - 1
    fill = photo[:, 6].view(B, 1, 1, 1) / 255.0
    w = F.grid_sample(x - fill, grid.view(B, S, S, 2), mode="bilinear", padding_mode="zeros", align_corners=False) + fill
    w = w.clamp(1e-4, 1)
    w = w ** torch.exp(photo[:, 0]).view(B, 1, 1, 1)
    inv = photo[:, 3].view(B, 1, 1, 1)
    w = w * (1 - inv) + (1 - w) * inv
    w = w * photo[:, 1].view(B, 1, 1, 1) + photo[:, 2].view(B, 1, 1, 1)
    for b in range(B):
        sg = float(photo[b, 4])
        if sg > 0:
            r = int(3 * sg) + 1
            k = torch.exp(-torch.arange(-r, r + 1, dtype=torch.float32, device=dv) ** 2 / (2 * sg * sg)); k = k / k.sum()
            wb = F.conv2d(F.pad(w[b:b + 1], (r, r, 0, 0), mode="replicate"), k.view(1, 1, 1, -1))
            w[b:b + 1] = F.conv2d(F.pad(wb, (0, 0, r, r), mode="replicate"), k.view(1, 1, -1, 1))
    w = w + torch.randn_like(w) * photo[:, 5].view(B, 1, 1, 1)
    w = (w.clamp(0, 1) * 255.0)
    return standardise(w)


def standardise(x):
    """Per-image standardisation, identical to normalise() and to the C++ runtime."""
    m = x.mean(dim=(1, 2, 3), keepdim=True)
    sd = x.std(dim=(1, 2, 3), keepdim=True, unbiased=False).clamp(min=4.0)
    return (x - m) / sd


# ------------------------------------------------------------------------- loss
def focal(pred_logit, gt):
    p = torch.sigmoid(pred_logit).clamp(1e-4, 1 - 1e-4)
    pos = gt.ge(0.99).float()
    neg = 1 - pos
    pos_loss = -torch.log(p) * (1 - p) ** 2 * pos
    neg_loss = -torch.log(1 - p) * p ** 2 * (1 - gt) ** 4 * neg
    n = pos.sum().clamp(min=1)
    return (pos_loss.sum() + neg_loss.sum()) / n


def loss_fn(out, tgt, mask, has):
    l_c = focal(out[:, 0], tgt[:, 0])
    l_k = focal(out[:, 1:5], tgt[:, 1:5])
    m = mask[:, None]
    n = mask.sum().clamp(min=1)
    l_off = (F.smooth_l1_loss(out[:, 5:13], tgt[:, 5:13], reduction="none", beta=0.05) * m).sum() / (8 * n)
    # sub-pixel offsets live at each corner's own cell
    kmask = (tgt[:, 13:21].abs() > 0).float()
    l_sub = (F.l1_loss(torch.sigmoid(out[:, 13:21]), tgt[:, 13:21], reduction="none") * kmask).sum() / kmask.sum().clamp(min=1)
    l_q = (F.binary_cross_entropy_with_logits(out[:, 21], tgt[:, 21], reduction="none") * mask).sum() / n
    l_v = (F.smooth_l1_loss(out[:, 22], tgt[:, 22], reduction="none", beta=0.05) * mask).sum() / n
    l_l = (F.binary_cross_entropy_with_logits(out[:, 23], tgt[:, 23], reduction="none") * mask).sum() / n
    total = l_c + l_k + 2.0 * l_off + l_sub + 0.5 * l_q + 0.5 * l_v + 0.3 * l_l
    return total, {"c": l_c.item(), "k": l_k.item(), "off": l_off.item(), "sub": l_sub.item(), "q": l_q.item(), "v": l_v.item(), "l": l_l.item()}


# ------------------------------------------------------------------------- decode (reference for C++)
def decode(out: np.ndarray, thr=0.3, top=4):
    """out (NCH,O,O) raw. Returns [{score, quad (4,2) input px, quality, version, print}]."""
    hm = 1 / (1 + np.exp(-out[0]))
    res = []
    pad = np.pad(hm, 1, constant_values=0)
    peaks = (hm >= pad[:-2, 1:-1]) & (hm >= pad[2:, 1:-1]) & (hm >= pad[1:-1, :-2]) & (hm >= pad[1:-1, 2:]) & (hm > thr)
    ys, xs = np.nonzero(peaks)
    order = np.argsort(-hm[ys, xs])[:top]
    for j in order:
        cj, ci = ys[j], xs[j]
        coarse = np.array([[ci * R + out[5 + 2 * k, cj, ci] * 16, cj * R + out[6 + 2 * k, cj, ci] * 16] for k in range(4)])
        side = np.mean([np.linalg.norm(coarse[k] - coarse[(k + 1) % 4]) for k in range(4)])
        rad = max(2, int(0.2 * side / R))
        quad = coarse.copy()
        for k in range(4):
            px, py = coarse[k] / R
            x0, x1 = int(max(0, px - rad)), int(min(O, px + rad + 1))
            y0, y1 = int(max(0, py - rad)), int(min(O, py + rad + 1))
            if x0 >= x1 or y0 >= y1:
                continue
            win = out[1 + k, y0:y1, x0:x1]
            yy, xx = np.unravel_index(np.argmax(win), win.shape)
            if 1 / (1 + np.exp(-win[yy, xx])) < 0.1:
                continue
            gx, gy = x0 + xx, y0 + yy
            sx, sy = 1 / (1 + np.exp(-out[13 + 2 * k, gy, gx])), 1 / (1 + np.exp(-out[14 + 2 * k, gy, gx]))
            quad[k] = [(gx + sx) * R, (gy + sy) * R]
        res.append({"score": float(hm[cj, ci]), "quad": quad, "quality": float(1 / (1 + np.exp(-out[21, cj, ci]))),
                    "version": float(out[22, cj, ci] * 8 + 16), "print": float(1 / (1 + np.exp(-out[23, cj, ci])))})
    return res


# ------------------------------------------------------------------------- train / eval
def load_recs(data_dir: Path):
    files = sorted(glob.glob(str(data_dir / "*.bin")))
    parts = [np.memmap(f, dtype=REC, mode="r") for f in files if Path(f).stat().st_size >= REC.itemsize]
    return np.concatenate(parts) if parts else np.zeros(0, REC)


def split(recs):
    """By case hash: ~5% validation, never the same case on both sides."""
    h = recs["hash"][:, 0].astype(int) * 256 + recs["hash"][:, 1]
    val = (h % 20) == 0
    return np.nonzero(~val)[0], np.nonzero(val)[0]


def corner_err(model, recs, idx, batch=32):
    model.eval()
    errs, found, qs = [], 0, []
    with torch.no_grad():
        for s in range(0, len(idx), batch):
            ii = idx[s:s + batch]
            x = torch.stack([torch.from_numpy(normalise(recs[i]["img"])[None]) for i in ii])
            dev = next(model.parameters()).device
            out = model(x.to(dev)).float().cpu().numpy()
            for j, i in enumerate(ii):
                if not recs[i]["has"]:
                    continue
                det = decode(out[j])
                q = recs[i]["quad"].reshape(4, 2)
                side = np.mean([np.linalg.norm(q[k] - q[(k + 1) % 4]) for k in range(4)])
                if not det:
                    errs.append(np.inf); continue
                best = min(det, key=lambda d: np.linalg.norm(d["quad"].mean(0) - q.mean(0)))
                e = np.max(np.linalg.norm(best["quad"] - q, axis=1)) / max(side, 1)
                errs.append(e); found += e < 0.1
                qs.append((best["quality"], int(recs[i]["gtq"])))
    errs = np.array(errs)
    fin = errs[np.isfinite(errs)]
    qa = np.array(qs) if qs else np.zeros((0, 2))
    acc = float(((qa[:, 0] > 0.5) == (qa[:, 1] > 0.5)).mean()) if len(qa) else float("nan")
    return {"n": len(errs), "found_within_10pct": float((errs < 0.1).mean()) if len(errs) else 0,
            "median_err_side": float(np.median(fin)) if len(fin) else float("nan"),
            "p90_err_side": float(np.percentile(fin, 90)) if len(fin) else float("nan"), "quality_acc": acc}


def pick_device() -> torch.device:
    if torch.backends.mps.is_available():
        return torch.device("mps")
    if torch.cuda.is_available():
        return torch.device("cuda")
    return torch.device("cpu")


def load_model(path: Path) -> "Locator":
    ck = torch.load(path, map_location="cpu")
    if isinstance(ck, dict) and "state" in ck:
        m = Locator(ck.get("width", 1.0)); m.load_state_dict(ck["state"])
    else:
        m = Locator(); m.load_state_dict(ck)
    return m


def train(data_dir: Path, out_dir: Path, epochs: int, batch: int, lr: float, workers: int, limit: int | None, width: float = 1.0):
    torch.manual_seed(0)
    dev = pick_device()
    print("device", dev, flush=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    recs = load_recs(data_dir)
    if limit:
        recs = recs[:limit]
    tr, va = split(recs)
    print(f"{len(recs)} records: {len(tr)} train, {len(va)} validation", flush=True)
    model = Locator(width).to(dev)
    print("parameters", sum(p.numel() for p in model.parameters()), flush=True)
    dl = torch.utils.data.DataLoader(Frames(recs[tr], True), batch_size=batch, shuffle=True, num_workers=workers, drop_last=True, persistent_workers=workers > 0)
    opt = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=1e-4)
    steps = epochs * len(dl)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=lr, total_steps=max(1, steps), pct_start=0.05)
    log = []
    best = None
    for ep in range(epochs):
        model.train()
        t0, agg, n = time.time(), {}, 0
        for img, M, photo, tgt, mask, has in dl:
            img, M, photo, tgt, mask = img.to(dev), M.to(dev), photo.to(dev), tgt.to(dev), mask.to(dev)
            out = model(apply_batch(img, M, photo))
            loss, parts = loss_fn(out, tgt, mask, has)
            opt.zero_grad(); loss.backward(); opt.step(); sched.step()
            for k, v in parts.items():
                agg[k] = agg.get(k, 0) + v
            n += 1
        ev = corner_err(model, recs, va[:2000])
        row = {"epoch": ep + 1, "sec": round(time.time() - t0), **{k: round(v / max(n, 1), 4) for k, v in agg.items()}, **ev}
        log.append(row)
        print(json.dumps(row), flush=True)
        sd = {"width": width, "state": {k: v.cpu() for k, v in model.state_dict().items()}}
        torch.save(sd, out_dir / "model.pt")
        if best is None or ev["found_within_10pct"] >= best:
            best = ev["found_within_10pct"]
            torch.save(sd, out_dir / "best.pt")
    (out_dir / "train_log.json").write_text(json.dumps(log, indent=1))


# ------------------------------------------------------------------------- export (portable)
def fold(seq: ConvBN):
    conv, bn = seq[0], seq[1]
    w = conv.weight.detach().double()
    scale = bn.weight.detach().double() / torch.sqrt(bn.running_var.double() + bn.eps)
    wf = w * scale[:, None, None, None]
    bf = bn.bias.detach().double() - bn.running_mean.double() * scale
    return wf.float(), bf.float(), conv.stride[0], conv.groups, len(seq) > 2


class GraphWriter:
    """CKML v2 (little-endian): 'CKML', u32 version=2, u32 n_buffers, u32 n_ops, u32 in_side,
    u32 out_stride, u32 out_channels, then ops:
      op 1 CONV  : u32 src, dst, cin, cout, k, stride, groups, relu; i8 weights (cout*cin/groups*k*k),
                   f32 scale (cout), f32 bias (cout).   w = i8 * scale[co]
      op 2 ADD   : u32 a, b, dst                        dst = a + b
      op 3 UP2ADD: u32 a, b, dst                        dst = nearest_up2(a) + b
    Buffer 0 is the standardised input (1 x S x S); the last op's dst is the output."""
    def __init__(self):
        self.ops, self.nbuf = [], 1

    def new(self):
        self.nbuf += 1
        return self.nbuf - 1

    def conv(self, src, w, b, stride, groups, relu):
        dst = self.new()
        cout, cin_g, k, _ = w.shape
        amax = w.abs().reshape(cout, -1).amax(dim=1).clamp(min=1e-12)
        scale = amax / 127.0
        q = torch.round(w / scale[:, None, None, None]).clamp(-127, 127).to(torch.int8)
        self.ops.append(struct.pack("<9I", 1, src, dst, cin_g * groups, cout, k, stride, groups, int(relu)) + q.numpy().tobytes()
                        + scale.numpy().astype("<f4").tobytes() + b.numpy().astype("<f4").tobytes())
        return dst

    def add(self, a, b, up=False):
        dst = self.new()
        self.ops.append(struct.pack("<4I", 3 if up else 2, a, b, dst))
        return dst

    def blob(self):
        return b"CKML" + struct.pack("<6I", 2, self.nbuf, len(self.ops), S, R, NCH) + b"".join(self.ops)


def export(model_path: Path, out_path: Path):
    model = load_model(model_path)
    model.eval()
    g = GraphWriter()
    def cb(seq, x):
        w, b, s_, gr, relu = fold(seq)
        return g.conv(x, w, b, s_, gr, relu)
    x = cb(model.stem, 0)
    feats = []
    for st in model.stages:
        for blk in st:
            t = x
            for c in blk.body:
                t = cb(c, t)
            x = g.add(x, t) if blk.res else t
        feats.append(x)
    y = cb(model.lat[3], feats[4])
    for i in (2, 1, 0):
        y = g.add(y, cb(model.lat[i], feats[i + 1]), up=True)
    for c in model.smooth:
        y = cb(c, y)
    y = cb(model.head[0], y)
    y = cb(model.head[1], y)
    hc = model.head[2]
    y = g.conv(y, hc.weight.detach().float(), hc.bias.detach().float(), 1, 1, False)
    blob = g.blob()
    out_path.write_bytes(blob)
    sha = hashlib.sha256(blob).hexdigest()
    print(f"wrote {out_path}: {len(g.ops)} ops, {g.nbuf} buffers, {len(blob)} bytes, sha256 {sha}")
    return sha


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("train"); t.add_argument("data", type=Path); t.add_argument("out", type=Path)
    t.add_argument("--epochs", type=int, default=12); t.add_argument("--batch", type=int, default=32)
    t.add_argument("--lr", type=float, default=2e-3); t.add_argument("--workers", type=int, default=3); t.add_argument("--limit", type=int)
    t.add_argument("--width", type=float, default=1.0, help="channel multiplier (1.0 = 0.41M weights)")
    e = sub.add_parser("export"); e.add_argument("model", type=Path); e.add_argument("out", type=Path)
    a = ap.parse_args()
    if a.cmd == "train":
        train(a.data, a.out, a.epochs, a.batch, a.lr, a.workers, a.limit, a.width)
    else:
        export(a.model, a.out)


if __name__ == "__main__":
    main()
