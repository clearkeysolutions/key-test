#!/usr/bin/env bash
# The ML site-model pipeline, end to end (Mac GPU via Metal, or Linux):
#   1. its own ckey checkout of the scratch branch (build 88 or later: the site reading)
#   2. fresh-seed plans (never the campaign's structured, adaptive or holdout seeds)
#   3. site training records with the simulator (half the cores by default, so it can
#      run next to ml/run_locator.sh)
#   4. training: wider model, fields the classical reader fails oversampled
#   5. the portable model (ml/locator-models/sites-<tag>.ckml) and its model card
#   6. classical vs site model on a separate fresh-seed set, identical frames
#
#   ml/run_sites.sh
#   CASES=60000 WIDTH=2.0 EPOCHS=20 HARD=3 JOBS=6 ml/run_sites.sh
# Finished steps are skipped, so it can be stopped and run again. Disk: about 120 KB per case.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
CASES=${CASES:-40000}
EVAL_CASES=${EVAL_CASES:-2000}
WIDTH=${WIDTH:-2.0}
EPOCHS=${EPOCHS:-20}
BATCH=${BATCH:-32}
HARD=${HARD:-3}
TAG=${TAG:-v1}
DATA=${DATA:-$ROOT/data/mlsites-$TAG}
CKEY=${CKEY:-$ROOT/data/ckey-scratch-sites}
NCPU=$( (sysctl -n hw.ncpu 2>/dev/null || nproc) )
JOBS=${JOBS:-$(( NCPU > 1 ? NCPU / 2 : 1 ))}
mkdir -p "$DATA" ml/locator-models

if [ ! -d "$CKEY/.git" ]; then
  echo "== cloning ckey (scratch) into $CKEY"
  git clone --depth 1 -b scratch https://github.com/clearkeysolutions/ckey "$CKEY"
fi
echo "== ckey checkout: $CKEY ($(git -C "$CKEY" rev-parse --short HEAD))"
grep -q ckey_ml_site_map "$CKEY/include/ckey/ckey.h" || { echo "ckey checkout predates the site reading (build 88); delete $CKEY and run again"; exit 1; }

echo "== building the worker"
cmake -S runners/native -B "$DATA/build" -DCMAKE_BUILD_TYPE=Release -DCKEY_SOURCE_DIR="$CKEY" >/dev/null
cmake --build "$DATA/build" -j "$JOBS" >/dev/null
W="$DATA/build/ckt_worker"
"$W" --selftest >/dev/null

echo "== python environment"
if [ ! -x "$DATA/venv/bin/python" ]; then
  python3 -m venv "$DATA/venv"
  "$DATA/venv/bin/pip" install -q --upgrade pip
  "$DATA/venv/bin/pip" install -q torch numpy pyyaml
fi
PY="$DATA/venv/bin/python"

echo "== plans"
[ -d "$DATA/plan" ] || "$PY" -m ml.frames_plan "$DATA/plan" "$CASES" --per-shard 1000 --tag "sites-$TAG"
[ -d "$DATA/evalplan" ] || "$PY" -m ml.frames_plan "$DATA/evalplan" "$EVAL_CASES" --per-shard 125 --tag "sites-$TAG-eval"

echo "== exporting site records ($JOBS in parallel; resumable)"
mkdir -p "$DATA/records"
export W DATA
ls "$DATA"/plan/shard-*.jsonl | xargs -n 1 -P "$JOBS" sh -c 'n=$(basename "$1" .jsonl); [ -f "$DATA/records/$n.done" ] || { rm -f "$DATA/records/$n.bin"; "$W" --ml-sites "$1" "$DATA/records/$n.bin" 2>>"$DATA/records/$n.log" && touch "$DATA/records/$n.done"; }' sh
echo "   $(ls "$DATA"/records/*.done | wc -l | tr -d ' ') shards done"

echo "== training (width $WIDTH, $EPOCHS epochs, hard fields x$HARD)"
if [ ! -f "$DATA/model/done" ]; then
  "$PY" -m ml.sites train "$DATA/records" "$DATA/model" --epochs "$EPOCHS" --batch "$BATCH" --workers 4 --width "$WIDTH" --hard-weight "$HARD"
  touch "$DATA/model/done"
fi

echo "== portable model"
OUT=ml/locator-models/sites-$TAG.ckml
SHA=$("$PY" -m ml.sites export "$DATA/model/best.pt" "$OUT" | sed -n 's/.*sha256 //p')

echo "== evaluation on a separate fresh-seed set (classical vs site model, identical frames)"
mkdir -p "$DATA/eval"
export OUT
ls "$DATA"/evalplan/shard-*.jsonl | xargs -n 1 -P "$JOBS" sh -c 'n=$(basename "$1" .jsonl); [ -f "$DATA/eval/$n.done" ] || { rm -f "$DATA/eval/$n.jsonl"; "$W" --ml-sites-compare "$1" "$OUT" "$DATA/eval/$n.jsonl" && touch "$DATA/eval/$n.done"; }' sh
"$PY" -m ml.sites_report "$DATA"/eval/*.jsonl --md "ml/locator-models/sites-$TAG.eval.md"

"$PY" - "$OUT" "$SHA" "$TAG" "$WIDTH" "$CASES" "$EPOCHS" "$HARD" "$(git -C "$CKEY" rev-parse HEAD)" "$DATA/model/train_log.json" <<'PY'
import json, sys, platform
out, sha, tag, width, cases, epochs, hard, ckey, log = sys.argv[1:10]
card = {"model": out, "sha256": sha, "tag": tag, "kind": "site model", "width": float(width), "architecture": "CKML v2 site model (ml/sites.py)",
        "training_cases": int(cases), "epochs": int(epochs), "hard_field_weight": float(hard), "plan_master_seed": f"CKEY-ML/frames/sites-{tag}",
        "eval_master_seed": f"CKEY-ML/frames/sites-{tag}-eval", "ckey_commit": ckey, "host": platform.platform(),
        "isolation": "Fresh seeds only: no campaign structured, adaptive or holdout case is used for training or evaluation.",
        "train_log": json.load(open(log))}
json.dump(card, open(out.replace(".ckml", ".card.json"), "w"), indent=1)
print("model card:", out.replace(".ckml", ".card.json"))
PY
echo "== done. Commit ml/locator-models/sites-$TAG.* and push to claude/gifted-edison-fjs8l3:"
echo "   git add ml/locator-models/sites-$TAG.* && git commit -m 'ML site model $TAG' && git push origin claude/gifted-edison-fjs8l3"
