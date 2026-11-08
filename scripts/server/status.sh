#!/usr/bin/env bash
# From your laptop: one-screen status of the server campaign.
HOST=${1:?usage: status.sh user@host}
ssh "$HOST" 'cd ~/key-test 2>/dev/null || { echo "not deployed"; exit 1; }
  ./ckt status | python3 -c "import json,sys
d=json.load(sys.stdin); print(\"state:\", d.pop(\"state\")); d.pop(\"data_dir\",None); d.pop(\"lock\",None)
for k,v in d.items(): print(f\"  {k:<22} {v[\"sealed\"]:>9,}/{v[\"planned\"]:<9,} wrong payloads {v[\"wrong_payloads\"]}\")"
  echo; tail -n 3 "$(ls -t campaign/logs/*.log | head -1)" 2>/dev/null; echo; uptime'
