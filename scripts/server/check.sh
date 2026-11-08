#!/usr/bin/env bash
# From your laptop: everything worth knowing about the server campaign in one screen.
#   scripts/server/check.sh ckeyrunner@62.238.99.101
HOST=${1:?usage: check.sh user@host}
ssh "$HOST" 'cd ~/key-test 2>/dev/null || { echo "not deployed"; exit 1; }; python3 - <<"PY"
import glob, json, os, shutil, time
def jl(p):
    try:
        return [json.loads(l) for l in open(p) if l.strip()]
    except OSError:
        return []
st = json.load(open("campaign/state/state.json"))["state"] if os.path.exists("campaign/state/state.json") else "NEW"
print(f"state: {st}")
total_wrong = 0
for mf in sorted(glob.glob("campaign/manifests/*.jsonl")):
    ps = os.path.basename(mf)[:-6]
    recs = jl(mf)
    planned = json.load(open(f"campaign/plans/{ps}/manifest.json"))["cases"] if os.path.exists(f"campaign/plans/{ps}/manifest.json") else 0
    sealed = sum(r["cases"] for r in recs)
    live = sum(sum(1 for l in open(f) if l.startswith("END ")) for f in glob.glob(f"campaign/progress/{ps}/*.log"))
    wrong = sum(r.get("wrong_payload", 0) for r in recs)
    total_wrong += wrong
    if ps.startswith("adaptive_bs_") and sealed == planned:
        continue
    print(f"  {ps:<18} {max(sealed, min(live, planned)):>10,} / {planned:<10,} sealed {sealed:>9,}   wrong payloads {wrong}")
print(f"WRONG PAYLOADS (all plan sets): {total_wrong}")
# Rate from the most active plan set (last 6 h of sealed shards).
recs = [r for mf in glob.glob("campaign/manifests/*.jsonl") if "pilot" not in mf for r in jl(mf)]
now = time.time()
recent = [r for r in recs if now - r["sealed_at"] < 6 * 3600]
if len(recent) > 3:
    span = max(r["sealed_at"] for r in recent) - min(r["sealed_at"] for r in recent)
    rate = sum(r["cases"] for r in recent[1:]) / max(1, span) * 3600
    s = jl("campaign/manifests/structured.jsonl")
    s_left = max(0, 1_000_000 - sum(r["cases"] for r in s))
    a_done = sum(r["cases"] for mf in glob.glob("campaign/manifests/adaptive_*.jsonl") for r in jl(mf))
    left = s_left + max(0, 500_000 - a_done)
    h = left / rate + 2
    fin = time.strftime("%a %H:%M UTC", time.gmtime(now + h * 3600))
    pst = time.strftime("%a %I:%M %p PT", time.gmtime(now + h * 3600 - 8 * 3600))
    print(f"rate: {rate:,.0f} cases/h  ->  all automated stages done in ~{h:.1f} h ({fin} = {pst})")
up = jl("campaign/storage/uploaded.jsonl")
gb = sum(u["bytes"] for u in up) / 1e9
print(f"R2: {len(up)} objects, {gb:.2f} GB uploaded; last sync log: ", end="")
try:
    print(open("campaign/logs/r2sync.log").read().strip().splitlines()[-1][-80:])
except (OSError, IndexError):
    print("none yet")
events = [e for f in glob.glob("campaign/infra/*.jsonl") for e in jl(f) if e.get("kind") not in ("r2_not_configured",)]
kinds = {}
for e in events:
    kinds[e["kind"]] = kinds.get(e["kind"], 0) + 1
print(f"problems logged: {kinds or "none"}")
du = shutil.disk_usage(".")
print(f"disk free: {du.free / 1e9:.0f} GB")
PY
echo "load: $(cut -d" " -f1-3 /proc/loadavg)   workers running: $(pgrep -c -f "[c]kt_worker")"
tmux has-session -t ckt 2>/dev/null && echo "tmux session: running" || echo "tmux session: NOT RUNNING"
grep -h -E "HALT|FAILED|Traceback" campaign/logs/server-*.log 2>/dev/null | tail -3'
