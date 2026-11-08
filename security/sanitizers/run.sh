#!/usr/bin/env bash
# Gate G6 evidence (SPEC §2.9, §10.6).
#  1. ASan + UBSan build (worker AND the production core) runs the selftest and
#     SAMPLE pilot cases; any sanitizer report or abnormal exit is a finding.
#  2. libFuzzer (clang + compiler-rt) runs fuzz_codeword and fuzz_gray8 for
#     MINUTES each in parallel; crashes, leaks, OOMs and timeouts are findings.
# Missing toolchain pieces are recorded as NOT_RUN, never as passes.
set -uo pipefail
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
SAMPLE=${1:-2000}
MIN=${2:-60}
OUT=campaign/reports/security
mkdir -p "$OUT"
GEN=""; command -v ninja >/dev/null && GEN="-G Ninja"
findings=0; sanitizer_status="NOT_RUN"; fuzz_status="NOT_RUN"; fuzz_crashes=0; fuzz_minutes=0; sanitizer_cases=0

echo "== ASan/UBSan build"
if cmake -S runners/native -B build/asan $GEN -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCKT_SANITIZE=ON -DCKEY_SOURCE_DIR="$ROOT/third_party/ckey" >/dev/null &&
   cmake --build build/asan -j "$(nproc)" >/dev/null; then
  export ASAN_OPTIONS="halt_on_error=1:detect_leaks=1:log_path=$ROOT/$OUT/asan"
  export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:log_path=$ROOT/$OUT/ubsan"
  rm -f "$OUT"/asan.* "$OUT"/ubsan.*
  ./build/asan/ckt_worker --selftest > "$OUT/selftest.log" 2>&1 || findings=$((findings+1))
  PLAN=$(ls campaign/plans/pilot/shard-*.jsonl.gz 2>/dev/null | head -100)
  if [ -n "$PLAN" ]; then
    zcat $PLAN | head -n "$SAMPLE" > "$OUT/sample.jsonl"
    split -n "l/$(nproc)" "$OUT/sample.jsonl" "$OUT/part_"
    pids=()
    for f in "$OUT"/part_*; do ./build/asan/ckt_worker --plan "$f" --results "$f.res" > /dev/null 2>&1 & pids+=($!); done
    for p in "${pids[@]}"; do wait "$p" || findings=$((findings+1)); done
    sanitizer_cases=$(cat "$OUT"/part_*.res 2>/dev/null | grep -c '"status":"COMPLETED"' || true)
    rm -f "$OUT"/part_*
  else
    echo "no pilot plan yet: sanitizer run covers the selftest only"
  fi
  reports=$(ls "$OUT"/asan.* "$OUT"/ubsan.* 2>/dev/null | wc -l)
  findings=$((findings+reports))
  sanitizer_status="RUN"
else
  echo "sanitizer build failed"; findings=$((findings+1)); sanitizer_status="BUILD_FAILED"
fi

echo "== libFuzzer"
if command -v clang++ >/dev/null && printf 'extern "C" int LLVMFuzzerTestOneInput(const unsigned char*,unsigned long){return 0;}' | clang++ -x c++ -fsanitize=fuzzer,address - -o /tmp/ckt-fuzz-probe 2>/dev/null; then
  if CXX=clang++ cmake -S runners/native -B build/fuzz $GEN -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCKT_FUZZ=ON -DCKEY_SOURCE_DIR="$ROOT/third_party/ckey" >/dev/null &&
     cmake --build build/fuzz -j "$(nproc)" --target fuzz_codeword fuzz_gray8 >/dev/null; then
    mkdir -p "$OUT/corpus_codeword" "$OUT/corpus_gray8" "$OUT/artifacts"
    (cd "$OUT/artifacts" && "$ROOT/build/fuzz/fuzz_codeword" -max_total_time=$((MIN*60)) -timeout=10 -rss_limit_mb=2048 -jobs=$(( $(nproc) / 2 > 0 ? $(nproc) / 2 : 1 )) "$ROOT/$OUT/corpus_codeword" > "$ROOT/$OUT/fuzz_codeword.log" 2>&1) &
    (cd "$OUT/artifacts" && "$ROOT/build/fuzz/fuzz_gray8" -max_total_time=$((MIN*60)) -timeout=10 -rss_limit_mb=2048 -jobs=$(( $(nproc) / 2 > 0 ? $(nproc) / 2 : 1 )) "$ROOT/$OUT/corpus_gray8" > "$ROOT/$OUT/fuzz_gray8.log" 2>&1) &
    wait
    fuzz_crashes=$(ls "$OUT"/artifacts/crash-* "$OUT"/artifacts/leak-* "$OUT"/artifacts/oom-* "$OUT"/artifacts/timeout-* 2>/dev/null | wc -l)
    fuzz_minutes=$MIN; fuzz_status="RUN"
  else
    fuzz_status="BUILD_FAILED"
  fi
else
  echo "clang with libFuzzer runtime not available (Ubuntu: apt install clang libclang-rt-dev); fuzzing NOT_RUN"
fi

python3 - "$OUT/../security.json" <<PY
import json, sys, time
json.dump({"time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "sanitizer_status": "$sanitizer_status",
           "sanitizer_findings": $findings, "sanitizer_cases": $sanitizer_cases, "fuzz_status": "$fuzz_status",
           "fuzz_minutes": $fuzz_minutes if "$fuzz_status" == "RUN" else 0, "fuzz_crashes": $fuzz_crashes,
           "evidence_dir": "campaign/reports/security"}, open(sys.argv[1], "w"), indent=1)
PY
cat "$OUT/../security.json"
