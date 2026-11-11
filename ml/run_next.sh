#!/usr/bin/env bash
# Round 2 on one machine, one after the other: the corner refiner, then the site model
# with its doubt channel (reusing round 1's site records). Each step is resumable.
#   caffeinate -i ml/run_next.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ml/run_refiner.sh
if [ -d data/mlsites-v1/records ]; then RECORDS=$(pwd)/data/mlsites-v1/records; else RECORDS=; fi
TAG=v2 DOUBT=1 WIDTH=${SITES_WIDTH:-2.5} RECORDS=${RECORDS:-} ml/run_sites.sh
echo
echo "== round 2 done. Push both:"
echo "   git add ml/locator-models/refiner-v1.* ml/locator-models/sites-v2.* && git commit -m 'ML round 2: corner refiner v1, site model v2 (doubt)' && git push origin claude/gifted-edison-fjs8l3"
