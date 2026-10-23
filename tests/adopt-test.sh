#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# End-to-end adoption test on an OpenWrt device. The system openmeshd
# instance acts as controller; a second manual instance acts as an
# unadopted agent (join_open). Run on the device:
#
#   scp tests/adopt-test.sh root@<device>:/tmp/ && ssh root@<device> sh /tmp/adopt-test.sh

set -eu

AGENT_DIR=/tmp/om-adopt
rm -rf "$AGENT_DIR"

openmeshd -d -s "$AGENT_DIR" -P 17805 -D 7689 -A 127.0.0.1 \
	-c 127.0.0.1 -j >/tmp/om-adopt.log 2>&1 &
AGENT_PID=$!
trap 'kill $AGENT_PID 2>/dev/null || true' EXIT

sleep 2

echo "== pending nodes =="
PENDING=$(ubus call openmesh list_pending)
echo "$PENDING"
ID=$(echo "$PENDING" | jsonfilter -e '@.nodes[0].id')
[ -n "$ID" ] || { echo "ADOPT-FAIL: agent not discovered" >&2; exit 1; }

echo "== adopt $ID =="
ubus call openmesh adopt "{ \"id\": \"$ID\" }"

# agent retries within 5s, then handshake + GRANT
sleep 7

echo "== adopted nodes =="
ADOPTED=$(ubus call openmesh list_adopted)
echo "$ADOPTED"
echo "$ADOPTED" | jsonfilter -e '@.nodes[0].online' | grep -q true || {
	echo "ADOPT-FAIL: node not online" >&2
	exit 1
}

[ -s "$AGENT_DIR/network.key" ] || {
	echo "ADOPT-FAIL: network.key missing on agent" >&2
	exit 1
}
grep -q "adopted by controller" /tmp/om-adopt.log || {
	echo "ADOPT-FAIL: agent log lacks adoption entry" >&2
	exit 1
}

echo "ADOPT-OK: discovery -> adopt -> encrypted session -> key delivered"
