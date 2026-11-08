#!/usr/bin/env bash
# Run on YOUR LAPTOP (inside a clone of key-test). Ships this exact commit to the rented
# server without giving the server any GitHub credentials, installs R2 credentials, and
# starts the campaign in a detached tmux session.
#
#   scripts/server/send.sh ckeyrunner@62.238.99.101 "Your Name"
#
# Re-running it is safe: it updates the code only if the campaign has not been locked yet,
# otherwise it just (re)attaches the run with scripts/resume.sh.
set -euo pipefail
HOST=${1:?usage: send.sh user@host "Approver Name"}
APPROVER=${2:?approver name required (records your approval of configs/gates.yaml)}
cd "$(dirname "$0")/../.."
[ -z "$(git status --porcelain -- configs simulation runners generator analysis ml storage scripts ckt)" ] || { echo "commit your changes first (the lock names an immutable commit)"; exit 1; }
BR=$(git rev-parse --abbrev-ref HEAD)
TMP=$(mktemp -d)
git bundle create "$TMP/key-test.bundle" "$BR"
echo "== copying bundle ($(du -h "$TMP/key-test.bundle" | cut -f1)) to $HOST"
scp -q "$TMP/key-test.bundle" "$HOST:/tmp/key-test.bundle"
if [ -f scripts/server/r2.env ]; then
  ssh "$HOST" 'mkdir -p ~/.config/ckey-test && chmod 700 ~/.config/ckey-test'
  scp -q scripts/server/r2.env "$HOST:.config/ckey-test/r2.env"
  ssh "$HOST" 'chmod 600 ~/.config/ckey-test/r2.env'
  echo "== R2 credentials installed"
else
  echo "WARNING: scripts/server/r2.env not found; results will stay on the server until you add it and run ./ckt sync"
fi
ssh "$HOST" bash -s -- "$BR" "$APPROVER" <<'REMOTE'
set -euo pipefail
BR=$1; APPROVER=$2
if [ -d ~/key-test/.git ]; then
  cd ~/key-test
  if [ -f campaign/state/lock.json ]; then
    echo "campaign already locked: not changing code"
  else
    git fetch -q /tmp/key-test.bundle "$BR:refs/remotes/bundle/$BR" && git checkout -q -B "$BR" "refs/remotes/bundle/$BR"
  fi
else
  git clone -q -b "$BR" /tmp/key-test.bundle ~/key-test
fi
cd ~/key-test
git log --oneline -1
command -v tmux >/dev/null || { echo "tmux missing: as root run  apt install -y tmux openssh-client"; exit 1; }
if tmux has-session -t ckt 2>/dev/null; then echo "tmux session 'ckt' already running"; exit 0; fi
tmux new -d -s ckt "bash -lc 'scripts/server/start.sh \"$APPROVER\"; exec bash'"
echo "started: tmux session 'ckt' on $(hostname)"
REMOTE
rm -rf "$TMP"
cat <<MSG

Started. Useful commands from your laptop:
  scripts/server/status.sh $HOST          # state, progress, ETA, wrong-payload count
  ssh -t $HOST tmux attach -t ckt         # watch live (detach: Ctrl-b d)
  scripts/server/fetch.sh $HOST           # pull small evidence/ back into this repo
Before the rental ends:
  ssh $HOST 'cd key-test && scripts/server/finish.sh'   # push everything to R2, export evidence
  scripts/server/fetch.sh $HOST && git add evidence && git commit -m "Campaign evidence" && git push
MSG
