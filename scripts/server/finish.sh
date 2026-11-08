#!/usr/bin/env bash
# ON THE SERVER, before the rental ends: upload everything to R2 (content-addressed),
# upload a checksum-verified archive bundle, export evidence. Deletes nothing.
set -euo pipefail
cd "$(dirname "$0")/../.."
[ -f ~/.config/ckey-test/r2.env ] && { set -a; . ~/.config/ckey-test/r2.env; set +a; }
./ckt sync
./ckt export-evidence
DEST=$(mktemp -d)
./ckt archive --dest "$DEST"
A=$(ls -d "$DEST"/ckey-test-archive-*)
tar czf "$A.tar.gz" -C "$DEST" "$(basename "$A")"
.venv/bin/python - "$A.tar.gz" <<'PY'
import sys; sys.path.insert(0, ".")
from pathlib import Path
from storage.r2 import R2
r = R2(Path("campaign")).put_cas(Path(sys.argv[1]), "archive")
print("archive in R2:", r["key"], f"{r['bytes']/1e9:.2f} GB", r["sha256"])
PY
echo "Done. Now on your laptop: scripts/server/fetch.sh <host>, commit and push evidence/."
echo "Only after verifying R2 + evidence should the server be released (SPEC §14)."
