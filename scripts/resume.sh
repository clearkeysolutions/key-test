#!/usr/bin/env bash
# Continue the campaign from its current state (idempotent; completed cases are never re-run).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p campaign/logs
LOG="campaign/logs/resume-$(date -u +%Y%m%dT%H%M%SZ).log"
exec > >(tee -a "$LOG") 2>&1
./ckt resume
[ -f campaign/reports/security.json ] || scripts/security.sh
./ckt status
