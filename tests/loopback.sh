#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Two-instance discovery smoke test. Run ON an OpenWrt device with the
# openmeshd package installed:
#
#   scp tests/loopback.sh root@<device>:/tmp/ && ssh root@<device> sh /tmp/loopback.sh
#
# DoD (roadmap M2): mutual discovery within 2 seconds.

set -eu

A_DIR=/tmp/om-lb-a
B_DIR=/tmp/om-lb-b
rm -rf "$A_DIR" "$B_DIR"

/usr/sbin/openmeshd -d -s "$A_DIR" -P 17801 -D 17802 -A 127.0.0.1 >/tmp/om-a.log 2>&1 &
PA=$!
/usr/sbin/openmeshd -d -s "$B_DIR" -P 17802 -D 17801 -A 127.0.0.1 >/tmp/om-b.log 2>&1 &
PB=$!
trap 'kill $PA $PB 2>/dev/null || true' EXIT

sleep 2

ok=0
grep -q "new node" /tmp/om-a.log && grep -q "new node" /tmp/om-b.log && ok=1
grep "node_id\|new node" /tmp/om-a.log | sed 's/^/[A] /'
grep "node_id\|new node" /tmp/om-b.log | sed 's/^/[B] /'

if [ "$ok" != 1 ]; then
	echo "LOOPBACK-FAIL: no mutual discovery within 2s" >&2
	exit 1
fi
echo "LOOPBACK-OK: mutual discovery within 2s"
