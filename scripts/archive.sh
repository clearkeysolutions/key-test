#!/usr/bin/env bash
# Copy all permanent assets (repo bundle, plans, results, traces, manifests,
# signatures, reports, retained images) to DEST with SHA256SUMS. Never deletes.
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -eq 1 ] || { echo "usage: scripts/archive.sh DEST_DIR"; exit 2; }
./ckt archive --dest "$1"
