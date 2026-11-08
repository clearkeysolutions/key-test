#!/usr/bin/env bash
# Runs ON THE SERVER inside tmux (started by send.sh): preflight, the whole automated
# campaign, ML, triage, security, R2 sync and evidence export. Resumes if re-run.
set -uo pipefail
cd "$(dirname "$0")/../.."
APPROVER=${1:-}
[ -f ~/.config/ckey-test/r2.env ] && { set -a; . ~/.config/ckey-test/r2.env; set +a; echo "R2: bucket ${R2_BUCKET:-ckey-hardening-test}"; }
mkdir -p campaign/logs
LOG="campaign/logs/server-$(date -u +%Y%m%dT%H%M%SZ).log"
exec > >(tee -a "$LOG") 2>&1
step() { echo; echo "######## $* ($(date -u +%H:%M:%SZ))"; }
step preflight
if ! { [ -f campaign/reports/preflight.json ] && grep -q '"ok": true' campaign/reports/preflight.json; }; then
  scripts/preflight.sh || { echo "PREFLIGHT FAILED: fix and re-run scripts/server/start.sh"; exit 1; }
fi
step "campaign (lock -> pilot -> 1,000,000 -> freeze -> 500,000)"
tries=0
until ./ckt run --approver "$APPROVER"; do
  rc=$?; tries=$((tries+1))
  [ $rc -eq 3 ] && { echo "CAMPAIGN HALTED (see above). Fix, then: scripts/server/start.sh"; exit 3; }
  [ $tries -ge 5 ] && { echo "run stopped $tries times (rc=$rc); not retrying. Inspect the log, then scripts/server/start.sh"; exit $rc; }
  echo "run stopped (rc=$rc); resuming in 60 s ($tries/5)"; sleep 60
done
step "ML localization option"; ./ckt ml || echo "ML step failed (campaign results unaffected)"
step triage; ./ckt triage || true
step security; [ -f campaign/reports/security.json ] || scripts/security.sh || true
step "R2 sync"; ./ckt sync || echo "R2 sync incomplete; re-run ./ckt sync"
step evidence; ./ckt export-evidence || true
step done; ./ckt status
echo "Automated stages finished. Fetch evidence from your laptop: scripts/server/fetch.sh <host>"
