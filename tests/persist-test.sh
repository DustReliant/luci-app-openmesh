#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# M5 persistence test on an OpenWrt device (after adopt-test.sh or a
# fresh agent): adopted list survives a controller restart, and the
# agent reconnects via the WELCOME path.
#
#   scp tests/persist-test.sh root@<device>:/tmp/ && ssh root@<device> sh /tmp/persist-test.sh

set -eu

AGENT_DIR=/tmp/om-adopt
[ -s "$AGENT_DIR/network.key" ] || {
	echo "PERSIST-FAIL: run adopt-test.sh first" >&2
	exit 1
}

echo "== UCI 落盘 =="
uci show openmesh | grep -E "node" | head -6

echo "== 重启控制器(系统实例) =="
/etc/init.d/openmeshd restart
sleep 2

AFTER=$(ubus call openmesh list_adopted)
echo "$AFTER"
N=$(echo "$AFTER" | jsonfilter -e '@.nodes[0].id' | wc -c)
[ "$N" -gt 2 ] || {
	echo "PERSIST-FAIL: adopted list lost after restart" >&2
	exit 1
}

echo "== agent 回归(WELCOME 路径, 不再 join_open) =="
openmeshd -d -s "$AGENT_DIR" -P 17805 -D 7689 -A 127.0.0.1 \
	-c 127.0.0.1 >/tmp/om-adopt2.log 2>&1 &
APID=$!
trap 'kill $APID 2>/dev/null || true' EXIT
sleep 7

ubus call openmesh list_adopted | jsonfilter -e '@.nodes[0].online' | \
	grep -q true || {
	echo "PERSIST-FAIL: node not online via WELCOME path" >&2
	exit 1
}
grep -q "controller link established" /tmp/om-adopt2.log || {
	echo "PERSIST-FAIL: agent did not re-establish" >&2
	exit 1
}

echo "PERSIST-OK: adopted persisted across restart, agent re-established"
