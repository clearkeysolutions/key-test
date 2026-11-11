#!/usr/bin/env bash
# The ML corner-refiner pipeline, end to end (Mac GPU via Metal, or Linux):
#   ckey scratch checkout (build 90 or later) -> fresh-seed plans -> refiner records
#   -> training -> ml/locator-models/refiner-<tag>.ckml + card -> classical vs refiner
#   on a separate fresh-seed set, identical frames.
#
#   ml/run_refiner.sh
#   CASES=60000 WIDTH=3.0 EPOCHS=25 JOBS=6 ml/run_refiner.sh
# Finished steps are skipped. Disk: about 16 KB per case.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
CASES=${CASES:-60000}
EVAL_CASES=${EVAL_CASES:-2000}
WIDTH=${WIDTH:-3.0}
EPOCHS=${EPOCHS:-25}
BATCH=${BATCH:-64}
TAG=${TAG:-v1}
DATA=${DATA:-$ROOT/data/mlrefine-$TAG}
CKEY=${CKEY:-$ROOT/data/ckey-scratch-sites}
NCPU=$( (sysctl -n hw.ncpu 2>/dev/null || nproc) )
JOBS=${JOBS:-$(( NCPU > 1 ? NCPU / 2 : 1 ))}
mkdir -p "$DATA" ml/locator-models

if [ ! -d "$CKEY/.git" ]; then
  echo "== cloning ckey (scratch) into $CKEY"
  git clone --depth 1 -b scratch https://github.com/clearkeysolutions/ckey "$CKEY"
else
  git -C "$CKEY" pull -q --ff-only origin scratch || echo "   (could not update $CKEY; using it as it is)"
fi
echo "== ckey checkout: $CKEY ($(git -C "$CKEY" rev-parse --short HEAD))"
grep -q ckey_ml_refiner_model_id "$CKEY/include/ckey/ckey.h" || { echo "ckey checkout predates the corner refiner (build 90); delete $CKEY and run again"; exit 1; }

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
[ -d "$DATA/plan" ] || "$PY" -m ml.frames_plan "$DATA/plan" "$CASES" --per-shard 1000 --tag "refine-$TAG"
[ -d "$DATA/evalplan" ] || "$PY" -m ml.frames_plan "$DATA/evalplan" "$EVAL_CASES" --per-shard 125 --tag "refine-$TAG-eval"

echo "== exporting refiner records ($JOBS in parallel; resumable)"
mkdir -p "$DATA/records"
export W DATA
ls "$DATA"/plan/shard-*.jsonl | xargs -n 1 -P "$JOBS" sh -c 'n=$(basename "$1" .jsonl); [ -f "$DATA/records/$n.done" ] || { rm -f "$DATA/records/$n.bin"; "$W" --ml-refine "$1" "$DATA/records/$n.bin" 2>>"$DATA/records/$n.log" && touch "$DATA/records/$n.done"; }' sh
echo "   $(ls "$DATA"/records/*.done | wc -l | tr -d ' ') shards done"

echo "== training (width $WIDTH, $EPOCHS epochs)"
if [ ! -f "$DATA/model/done" ]; then
  "$PY" -m ml.refiner train "$DATA/records" "$DATA/model" --epochs "$EPOCHS" --batch "$BATCH" --workers 4 --width "$WIDTH"
  touch "$DATA/model/done"
fi

echo "== portable model"
OUT=ml/locator-models/refiner-$TAG.ckml
SHA=$("$PY" -m ml.refiner export "$DATA/model/best.pt" "$OUT" | sed -n 's/.*sha256 //p')

echo "== evaluation on a separate fresh-seed set (classical vs refiner, identical frames)"
mkdir -p "$DATA/eval"
export OUT
ls "$DATA"/evalplan/shard-*.jsonl | xargs -n 1 -P "$JOBS" sh -c 'n=$(basename "$1" .jsonl); [ -f "$DATA/eval/$n.done" ] || { rm -f "$DATA/eval/$n.jsonl"; "$W" --ml-refine-compare "$1" "$OUT" "$DATA/eval/$n.jsonl" && touch "$DATA/eval/$n.done"; }' sh
"$PY" -m ml.refine_report "$DATA"/eval/*.jsonl --md "ml/locator-models/refiner-$TAG.eval.md"

"$PY" - "$OUT" "$SHA" "$TAG" "$WIDTH" "$CASES" "$EPOCHS" "$(git -C "$CKEY" rev-parse HEAD)" "$DATA/model/train_log.json" <<'PY'
import json, sys, platform
out, sha, tag, width, cases, epochs, ckey, log = sys.argv[1:9]
card = {"model": out, "sha256": sha, "tag": tag, "kind": "corner refiner", "width": float(width), "architecture": "CKML v2 corner refiner (ml/refiner.py)",
        "training_cases": int(cases), "epochs": int(epochs), "plan_master_seed": f"CKEY-ML/frames/refine-{tag}",
        "eval_master_seed": f"CKEY-ML/frames/refine-{tag}-eval", "ckey_commit": ckey, "host": platform.platform(),
        "isolation": "Fresh seeds only: no campaign structured, adaptive or holdout case is used for training or evaluation.",
        "train_log": json.load(open(log))}
json.dump(card, open(out.replace(".ckml", ".card.json"), "w"), indent=1)
print("model card:", out.replace(".ckml", ".card.json"))
PY
echo "== done. Commit ml/locator-models/refiner-$TAG.* and push to claude/gifted-edison-fjs8l3:"
echo "   git add ml/locator-models/refiner-$TAG.* && git commit -m 'ML corner refiner $TAG' && git push origin claude/gifted-edison-fjs8l3"
