#!/usr/bin/env bash
# One command for the whole automated campaign (SPEC §15 phases 0-4):
#   preflight -> BASELINE_LOCKED (your approval) -> plans -> pilot -> 1,000,000
#   -> classify -> REPORT_1M (frozen, signed) -> weaknesses -> 500,000 adaptive
#   -> security (sanitizers + fuzzing).
# Resumable: if it stops for any reason, run scripts/resume.sh.
# Run it inside tmux or screen; it takes days. Logs: campaign/logs/.
set -euo pipefail
cd "$(dirname "$0")/.."
APPROVER=""
while [ $# -gt 0 ]; do case "$1" in --approver) APPROVER="$2"; shift 2;; *) echo "unknown argument $1"; exit 2;; esac; done
mkdir -p campaign/logs
LOG="campaign/logs/run-$(date -u +%Y%m%dT%H%M%SZ).log"
exec > >(tee -a "$LOG") 2>&1
echo "log: $LOG"
[ -f campaign/reports/preflight.json ] && grep -q '"ok": true' campaign/reports/preflight.json || scripts/preflight.sh
if [ "$(./ckt status | python3 -c 'import json,sys;print(json.load(sys.stdin)["state"])')" = "NEW" ] && [ -z "$APPROVER" ]; then
  echo 'The first run records your approval of configs/gates.yaml: scripts/run.sh --approver "Your Name"'; exit 2
fi
./ckt run ${APPROVER:+--approver "$APPROVER"}
scripts/security.sh
./ckt status
