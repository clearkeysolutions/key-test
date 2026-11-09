#!/usr/bin/env bash
# From your laptop: interim results from the shards sealed so far (read-only, low priority).
# Not the frozen report: numbers move until REPORT_1M. Takes ~1-2 minutes.
#   scripts/server/peek.sh ckeyrunner@62.238.99.101 [plan_set]
HOST=${1:?usage: peek.sh user@host [plan_set]}
PS=${2:-structured}
ssh "$HOST" "cd ~/key-test && nice -n 19 python3 - $PS" <<'PY'
import glob, gzip, json, math, sys
from collections import defaultdict
ps = sys.argv[1]
def wilson(k, n, z=1.96):
    if not n:
        return (0, 0)
    p = k / n; d = 1 + z * z / n; c = (p + z * z / (2 * n)) / d; h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (max(0.0, 100 * (c - h)), min(100.0, 100 * (c + h)))
def pct(k, n):
    if not n:
        return "     -          "
    lo, hi = wilson(k, n)
    return f"{100*k/n:5.1f}% [{lo:4.1f},{hi:5.1f}]"
names = {"S01": "clean / ideal", "S02": "payload & patterns", "S03": "scale (px/module)", "S04": "perspective / bend", "S05": "rotation", "S06": "defocus",
         "S07": "motion blur", "S08": "low light / noise", "S09": "contrast & shadows", "S10": "glare", "S11": "occlusion / damage", "S12": "print ink & paper",
         "S13": "screens", "S14": "compression / ISP", "S15": "colour", "S16": "clutter / 2 codes", "S17": "ring stress", "S18": "ray aliasing",
         "S19": "corners / distortion", "S20": "combined + video"}
st = defaultdict(lambda: [0, 0, 0, 0]); band = defaultdict(lambda: [0, 0]); var = defaultdict(lambda: [0, 0])
bins = defaultdict(lambda: [0, 0]); reasons = defaultdict(int); tri = [0, 0]; neg = defaultdict(lambda: [0, 0, 0])
clean = defaultdict(lambda: [0, 0]); n_all = 0; wrong = 0; ppm_fail = defaultdict(lambda: [0, 0])
files = sorted(glob.glob(f"campaign/results/{ps}/shard-*.jsonl.gz"))
for f in files:
    for line in gzip.open(f, "rt"):
        try:
            r = json.loads(line)
        except Exception:
            continue
        if r.get("status") != "COMPLETED":
            continue
        n_all += 1
        wrong += bool(r.get("wrong_payload"))
        v = "P0.1" if str(r.get("format_version", "")).startswith("P0.1") else "P0"
        if r.get("neg_kind", "none") != "none":
            k = neg[r["neg_kind"]]; k[0] += 1; k[1] += r.get("observed_outcome") == "NO_READ"; k[2] += bool(r.get("wrong_payload"))
            continue
        ok = bool(r.get("success")); rec = r.get("phys_class") == "RECOVERABLE"; s = r.get("stratum")
        a = st[s]; a[0] += 1; a[1] += ok; a[2] += rec; a[3] += ok and rec
        if rec:
            ver = r.get("p0_version") or 0
            b = "v1-8" if ver <= 8 else "v9-16" if ver <= 16 else "v17-24" if ver <= 24 else "v25-31"
            band[b][0] += 1; band[b][1] += ok
            var[v][0] += 1; var[v][1] += ok
            bins[(s, r.get("sev_bin"))][0] += 1; bins[(s, r.get("sev_bin"))][1] += ok
            pp = r.get("ppm_min") or 0
            pb = "<2" if pp < 2 else "2-3" if pp < 3 else "3-4" if pp < 4 else "4-6" if pp < 6 else "6-8" if pp < 8 else ">=8"
            ppm_fail[pb][0] += 1; ppm_fail[pb][1] += ok
        if not ok:
            reasons[r.get("failure_reason")] += 1
            if r.get("gt_quad_decodes") in (0, 1) and r.get("phys_class") != "UNRECOVERABLE":
                tri[r["gt_quad_decodes"]] += 1
        if s == "S01" and (r.get("ppm_min") or 0) >= 6 and r.get("phys_class") != "UNRECOVERABLE":
            clean[v][0] += 1; clean[v][1] += ok
print(f"== {ps}: {n_all:,} cases in {len(files)} sealed shards (interim; final numbers come with REPORT_1M)")
print(f"WRONG PAYLOADS: {wrong}   (95% upper bound on the rate: {3/max(1,n_all)*1e6:.1f} per million)")
print("\nDecode success by stratum      all valid cases          physically recoverable only")
for s in sorted(st):
    a = st[s]
    print(f"  {s} {names.get(s, ''):<22} {pct(a[1], a[0])}   {pct(a[3], a[2])}  (n={a[2]:,})")
tot = [sum(x[i] for x in st.values()) for i in range(4)]
print(f"  ALL                         {pct(tot[1], tot[0])}   {pct(tot[3], tot[2])}  (n={tot[2]:,})")
print("\nRecoverable cases by camera px/module (the QR-parity question):")
for k in ("<2", "2-3", "3-4", "4-6", "6-8", ">=8"):
    print(f"  {k:>5} px/module  {pct(ppm_fail[k][1], ppm_fail[k][0])}  (n={ppm_fail[k][0]:,})")
print("\nRecoverable cases by density (P0 version):  " + "   ".join(f"{b} {100*band[b][1]/max(1,band[b][0]):.1f}%" for b in ("v1-8", "v9-16", "v17-24", "v25-31")))
print("Recoverable cases by variant:               " + "   ".join(f"{k} {100*x[1]/max(1,x[0]):.1f}% (n={x[0]:,})" for k, x in sorted(var.items())))
print("\nClean S01 at >= 6 px/module (claimed minimum; gate G2 wants 100%):  " + "   ".join(f"{k} {x[1]:,}/{x[0]:,} = {100*x[1]/max(1,x[0]):.2f}%" for k, x in sorted(clean.items())))
print("\nWhy valid symbols fail:")
for k, n in sorted(reasons.items(), key=lambda kv: -kv[1])[:8]:
    print(f"  {k:<22} {n:>8,}")
t = sum(tri)
if t:
    print(f"\nOf physically readable failures with a true-corner retest: {100*tri[1]/t:.1f}% DO decode given the true corners")
    print("  -> those are corner-finding / quad-fitting failures; the rest fail in sampling or decoding")
print("\nHard negatives (must never return a wrong payload):")
for k, x in sorted(neg.items()):
    print(f"  {k:<18} {x[0]:>7,} cases   rejected {100*x[1]/max(1,x[0]):5.1f}%   wrong {x[2]}")
print("\nWorst condition cells (stratum, severity bin 0=easiest), recoverable cases, n>=200:")
worst = sorted([(x[1] / x[0], k, x) for k, x in bins.items() if x[0] >= 200])[:6]
for r_, k, x in worst:
    print(f"  {k[0]} bin {k[1]}  {100*r_:5.1f}%  (n={x[0]:,})")
PY
