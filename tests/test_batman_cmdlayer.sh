#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Command-layer tests for om_batman_collect (r21b acceptance 16/17/18):
#   16 success        -- fake batctl emits a real captured fixture line
#   17 timeout        -- fake batctl sleeps forever, timeout kills it
#   18 command failure-- no batctl in PATH at all
# Plus: no zombie children survive any case, and the timeout case
# returns in bounded time (main-loop stall == configured timeout).
#
# Runs on the host (make sure tests/host-build.sh compiled the harness).

set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=$REPO/tests/.build
FAKE=$BIN/fakebin
OUT=$BIN/.bat_cmdlayer_out
HARNESS=$BIN/test_batman_collect

[ -x "$HARNESS" ] || { echo "harness missing: $HARNESS"; exit 1; }

fail() { echo "CMDLAYER-FAIL: $*" >&2; exit 1; }

mkdir -p "$FAKE"
rm -f "$OUT"

# 16. success: fake batctl replays the real captured fixture
cat > "$FAKE/batctl" <<EOF
#!/bin/sh
cat "$REPO/tests/fixtures/batctl-originators-normal-2024.3.txt"
EOF
chmod +x "$FAKE/batctl"

PATH="$FAKE:$PATH" "$HARNESS" om_bat 1 "$OUT" || fail "16 success rc"
grep -q "ec:2e:98:d2:71:28" "$OUT" || fail "16 fixture content"

# 17. timeout: batctl hangs, must be killed within the bound
cat > "$FAKE/batctl" <<'EOF'
#!/bin/sh
sleep 120
EOF
chmod +x "$FAKE/batctl"

START=$(date +%s)
if PATH="$FAKE:$PATH" "$HARNESS" om_bat 2 "$OUT" 2>/dev/null; then
	fail "17 timeout unexpectedly succeeded"
fi
ELAPSED=$(( $(date +%s) - START ))
[ "$ELAPSED" -le 4 ] || fail "17 timeout took ${ELAPSED}s (unbounded?)"
[ "$ELAPSED" -ge 2 ] || fail "17 returned too fast (${ELAPSED}s, killed early?)"

# 18. command failure: no batctl anywhere
rm -f "$FAKE/batctl"
if PATH="$FAKE" "$HARNESS" om_bat 1 "$OUT" 2>/dev/null; then
	fail "18 missing-command unexpectedly succeeded"
fi

# zombie check: no leftover sleep/batctl from the timeout case
ps -eo stat,cmd | awk '$1 ~ /Z/ && /batctl|sleep/' | grep . && \
	fail "zombie children left behind"

# the shell redirect truncates the file BEFORE batctl runs, so a timed-
# out collect leaves it empty -- retention is therefore IN MEMORY
# (monitor batman_stage table), never the staging file. Assert exactly
# that contract: empty file + non-success return.
[ ! -s "$OUT" ] || fail "timed-out collect left stale file (retention is monitor-side, in RAM)"

echo "CMDLAYER-OK: success/timeout($ELAPSED"s"/failure + no zombies + retention intact"
