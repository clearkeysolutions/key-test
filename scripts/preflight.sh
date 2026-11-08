#!/usr/bin/env bash
# Preflight (SPEC §8, §12): toolchain checks, pinned ckey checkout, hermetic-ish
# Python env, Release build, selftest, config/schema consistency, and a
# throughput benchmark with an ETA and disk estimate for THIS machine.
# Safe to re-run. Writes campaign/reports/preflight.json.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
say() { printf '\n== %s\n' "$*"; }
fail() { printf '\nPREFLIGHT FAILED: %s\n' "$*" >&2; exit 1; }

say "toolchain"
for t in git cmake python3; do command -v $t >/dev/null || fail "$t not found (Ubuntu: apt install git cmake python3 python3-venv build-essential)"; done
CXX_BIN=${CXX:-$(command -v g++ || command -v clang++ || true)}
[ -n "$CXX_BIN" ] || fail "no C++20 compiler (apt install build-essential)"
cmake_ver=$(cmake --version | head -1 | awk '{print $3}')
python3 - "$cmake_ver" <<'PY' || fail "cmake >= 3.20 required"
import sys; v=tuple(int(x) for x in sys.argv[1].split('.')[:2]); sys.exit(0 if v>=(3,20) else 1)
PY
python3 -c 'import sys; sys.exit(0 if sys.version_info>=(3,9) else 1)' || fail "python >= 3.9 required"
command -v ssh-keygen >/dev/null || echo "WARNING: ssh-keygen missing: artifacts will be recorded UNSIGNED (apt install openssh-client)"
GEN=""; command -v ninja >/dev/null && GEN="-G Ninja"
echo "cmake $cmake_ver, $($CXX_BIN --version | head -1), $(python3 --version), cpus $(nproc), mem $(free -g | awk '/Mem/{print $2}') GB"

say "pinned ckey checkout"
COMMIT=$(python3 -c "import re;print(re.search(r'commit:\s*([0-9a-f]{40})', open('configs/baseline.yaml').read()).group(1))")
mkdir -p third_party
if [ ! -d third_party/ckey/.git ]; then
  git clone --filter=blob:none https://github.com/clearkeysolutions/ckey third_party/ckey
fi
git -C third_party/ckey fetch --quiet origin "$COMMIT" 2>/dev/null || git -C third_party/ckey fetch --quiet origin
git -C third_party/ckey -c advice.detachedHead=false checkout --quiet "$COMMIT"
[ "$(git -C third_party/ckey rev-parse HEAD)" = "$COMMIT" ] || fail "ckey checkout is not $COMMIT"
[ -z "$(git -C third_party/ckey status --porcelain)" ] || fail "third_party/ckey has local changes"
echo "ckey at $COMMIT"

say "python environment (.venv)"
if [ ! -x .venv/bin/python ]; then
  python3 -m venv .venv || fail "python3 -m venv failed (Ubuntu: apt install python3-venv)"
fi
.venv/bin/python -m pip install --quiet --disable-pip-version-check -r requirements.txt

say "build (Release, -ffp-contract=off for cross-machine reproducible frames)"
cmake -S runners/native -B build/native $GEN -DCMAKE_BUILD_TYPE=Release -DCKEY_SOURCE_DIR="$ROOT/third_party/ckey" >/dev/null
cmake --build build/native -j "$(nproc)"

say "worker selftest"
./build/native/ckt_worker --selftest | tee /tmp/ckt-selftest.$$ | grep -E "FAIL|INFO|SELFTEST"
grep -q "SELFTEST PASSED" /tmp/ckt-selftest.$$ || fail "selftest failed"
rm -f /tmp/ckt-selftest.$$

say "configuration / schema consistency and throughput benchmark"
CKT_NO_VENV=1 .venv/bin/python scripts/preflight_check.py "$@"
