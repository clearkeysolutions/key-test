#!/usr/bin/env bash
# Gate G6 evidence: ASan/UBSan build of the worker + core running the selftest
# and a pilot sample, and libFuzzer runs of the decoders (clang required).
# Writes campaign/reports/security.json.
set -euo pipefail
cd "$(dirname "$0")/.."
MIN=${FUZZ_MINUTES:-60}
SAMPLE=${SANITIZER_CASES:-2000}
exec security/sanitizers/run.sh "$SAMPLE" "$MIN"
