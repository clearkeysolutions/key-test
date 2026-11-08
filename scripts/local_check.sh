#!/usr/bin/env bash
# The CI smoke check, run on your own machine (macOS or Linux) instead of GitHub Actions:
# pinned ckey checkout, Python env, Release build, selftest, and a 40-case smoke run of the
# real worker -> supervisor -> results DB -> report. Takes a few minutes. Touches nothing on GitHub.
#   macOS needs: xcode-select --install ; brew install cmake ninja python
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
for t in git cmake python3 c++; do command -v $t >/dev/null || { echo "missing $t (macOS: xcode-select --install; brew install cmake ninja python)"; exit 1; }; done
COMMIT=$(grep -oE '[0-9a-f]{40}' configs/baseline.yaml | head -1)
[ -d third_party/ckey/.git ] || git clone -q --filter=blob:none https://github.com/clearkeysolutions/ckey third_party/ckey
git -C third_party/ckey fetch -q origin "$COMMIT" 2>/dev/null || true
git -C third_party/ckey -c advice.detachedHead=false checkout -q "$COMMIT"
[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/python -m pip install -q --disable-pip-version-check -r requirements.txt
GEN=""; command -v ninja >/dev/null && GEN="-G Ninja"
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
cmake -S runners/native -B build/native $GEN -DCMAKE_BUILD_TYPE=Release -DCKEY_SOURCE_DIR="$ROOT/third_party/ckey" >/dev/null
cmake --build build/native -j "$JOBS"
./build/native/ckt_worker --selftest | grep -E "FAIL|SELFTEST"
./ckt smoke --cases "${1:-40}"
echo "LOCAL CHECK PASSED"
