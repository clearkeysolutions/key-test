#!/usr/bin/env bash
# Offline artifact verification: every sealed shard's hashes, the lock and
# freeze signatures, and bit-for-bit re-execution of a random sample of cases.
set -euo pipefail
cd "$(dirname "$0")/.."
./ckt verify --sample "${1:-50}"
