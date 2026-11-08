#!/usr/bin/env bash
# From your laptop: copy the small evidence/ directory (hashes, manifests, reports,
# counterexample case lines, gzipped SVGs, ML model) from the server into this repo.
set -euo pipefail
HOST=${1:?usage: fetch.sh user@host}
cd "$(dirname "$0")/../.."
ssh "$HOST" 'cd ~/key-test && ./ckt export-evidence >/dev/null && tar czf - evidence' | tar xzf -
du -sh evidence; git status --short evidence | head
echo "review, then: git add evidence && git commit -m 'Campaign evidence' && git push"
