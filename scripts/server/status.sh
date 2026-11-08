#!/usr/bin/env bash
# From your laptop: one-screen status of the server campaign.
HOST=${1:?usage: status.sh user@host}
ssh "$HOST" 'cd ~/key-test 2>/dev/null || { echo "not deployed"; exit 1; }
  ./ckt status | python3 -c "import json,sys
d=json.load(sys.stdin); print(\"state:\", d.pop(\"state\")); d.pop(\"data_dir\",None); d.pop(\"lock\",None)
for k,v in d.items(): print(f\"  {k:<22} {v[\"sealed\"]:>9,}/{v[\"planned\"]:<9,} wrong payloads {v[\"wrong_payloads\"]}\")"
  for d in campaign/progress/*/; do n=$(cat "$d"*.log 2>/dev/null | grep -c ^END); [ "$n" -gt 0 ] && echo "  live (incl. unsealed): $(basename "$d") $n"; done
  echo "  R2 objects uploaded: $(wc -l < campaign/storage/uploaded.jsonl 2>/dev/null || echo 0)"
  echo; uptime'
