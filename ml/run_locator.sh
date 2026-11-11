#!/usr/bin/env bash
# The ML locator pipeline, end to end, on a Mac (Apple-silicon GPU via Metal) or Linux:
#   1. build the harness worker against a ckey checkout (default: the scratch branch)
#   2. generate a fresh-seed plan (never the campaign's structured, adaptive or holdout seeds)
#   3. export training frames with the simulator, all cores in parallel
#   4. train the network (GPU when available)
#   5. write the portable model (ml/locator-models/locator-<tag>.ckml) and its model card
#   6. evaluate classical vs ML on a separate fresh-seed set, identical frames
#
#   ml/run_locator.sh                       # defaults: 80,000 training cases, width 1.0
#   CASES=120000 WIDTH=1.5 EPOCHS=30 ml/run_locator.sh
#   CKEY=~/src/ckey ml/run_locator.sh       # use an existing ckey checkout
#
# Steps that already finished are skipped, so it can be stopped and run again.
# Needs: git, cmake, a C++20 compiler, python3 (3.10+). Disk: about 70 KB per case.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
CASES=${CASES:-80000}
EVAL_CASES=${EVAL_CASES:-4000}
WIDTH=${WIDTH:-1.0}
EPOCHS=${EPOCHS:-25}
BATCH=${BATCH:-64}
TAG=${TAG:-v1}
DATA=${DATA:-$ROOT/data/mlframes-$TAG}
CKEY=${CKEY:-$ROOT/data/ckey-scratch}
JOBS=${JOBS:-$( (sysctl -n hw.ncpu 2>/dev/null || nproc) )}
mkdir -p "$DATA"

if [ ! -d "$CKEY/.git" ]; then
  echo "== cloning ckey (scratch) into $CKEY"
  git clone --depth 1 -b scratch https://github.com/clearkeysolutions/ckey "$CKEY"
else
  echo "== ckey checkout: $CKEY ($(git -C "$CKEY" rev-parse --short HEAD))"
fi

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
[ -d "$DATA/plan" ] || "$PY" -m ml.frames_plan "$DATA/plan" "$CASES" --per-shard 2000 --tag "$TAG"
[ -d "$DATA/evalplan" ] || "$PY" -m ml.frames_plan "$DATA/evalplan" "$EVAL_CASES" --per-shard 250 --tag "$TAG-eval"

echo "== exporting training frames ($JOBS in parallel; resumable)"
mkdir -p "$DATA/frames"
export W DATA
ls "$DATA"/plan/shard-*.jsonl | xargs -P "$JOBS" -I{} sh -c 'n=$(basename {} .jsonl); [ -f "$DATA/frames/$n.done" ] || { rm -f "$DATA/frames/$n.bin"; "$W" --ml-frames {} "$DATA/frames/$n.bin" 2>>"$DATA/frames/$n.log" && touch "$DATA/frames/$n.done"; }'
echo "   $(ls "$DATA"/frames/*.done | wc -l | tr -d ' ') shards done"

echo "== training (width $WIDTH, $EPOCHS epochs)"
if [ ! -f "$DATA/model/done" ]; then
  "$PY" -m ml.locator train "$DATA/frames" "$DATA/model" --epochs "$EPOCHS" --batch "$BATCH" --workers 6 --width "$WIDTH"
  touch "$DATA/model/done"
fi

echo "== portable model"
mkdir -p ml/locator-models
OUT=ml/locator-models/locator-$TAG.ckml
SHA=$("$PY" -m ml.locator export "$DATA/model/best.pt" "$OUT" | sed -n 's/.*sha256 //p')

echo "== evaluation on a separate fresh-seed set (classical vs ML, identical frames)"
mkdir -p "$DATA/eval"
export OUT
ls "$DATA"/evalplan/shard-*.jsonl | xargs -P "$JOBS" -I{} sh -c 'n=$(basename {} .jsonl); [ -f "$DATA/eval/$n.done" ] || { rm -f "$DATA/eval/$n.jsonl"; "$W" --ml-compare {} "$OUT" "$DATA/eval/$n.jsonl" && touch "$DATA/eval/$n.done"; }'
"$PY" -m ml.compare_report "$DATA"/eval/*.jsonl --md "ml/locator-models/locator-$TAG.eval.md"

"$PY" - "$OUT" "$SHA" "$TAG" "$WIDTH" "$CASES" "$EPOCHS" "$(git -C "$CKEY" rev-parse HEAD)" "$DATA/model/train_log.json" <<'PY'
import json, sys, platform
out, sha, tag, width, cases, epochs, ckey, log = sys.argv[1:9]
card = {"model": out, "sha256": sha, "tag": tag, "width": float(width), "architecture": "CKML v2 locator (ml/locator.py)",
        "training_cases": int(cases), "epochs": int(epochs), "plan_master_seed": f"CKEY-ML/frames/{tag}",
        "eval_master_seed": f"CKEY-ML/frames/{tag}-eval", "ckey_commit": ckey, "host": platform.platform(),
        "isolation": "Fresh seeds only: no campaign structured, adaptive or holdout case is used for training or evaluation.",
        "train_log": json.load(open(log))}
json.dump(card, open(out.replace(".ckml", ".card.json"), "w"), indent=1)
print("model card:", out.replace(".ckml", ".card.json"))
PY
echo "== done. Commit ml/locator-models/locator-$TAG.* and push to claude/gifted-edison-fjs8l3:"
echo "   git add ml/locator-models/locator-$TAG.* && git commit -m 'ML locator $TAG' && git push origin claude/gifted-edison-fjs8l3"
