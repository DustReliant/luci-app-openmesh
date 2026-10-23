#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Combined M4+M5 end-to-end test on an OpenWrt device:
#   discovery -> adopt -> encrypted session -> UCI persistence ->
#   controller restart -> WELCOME reconnection.
#
#   scp tests/e2e-m45.sh root@<device>:/tmp/ && ssh root@<device> sh /tmp/e2e-m45.sh

set -eu

AGENT_DIR=/tmp/om-e2e

fail() { echo "E2E-FAIL: $*" >&2; exit 1; }

wait_ubus() {	# wait until the system daemon serves the openmesh object
	i=0
	while [ $i -lt 20 ]; do
		ubus call openmesh status >/dev/null 2>&1 && return 0
		i=$((i + 1))
		sleep 1
	done
	return 1
}

wait_online() {	# wait until node[0].online == true
	i=0
	while [ $i -lt 20 ]; do
		if ubus call openmesh list_adopted 2>/dev/null | \
			jsonfilter -e '@.nodes[0].online' 2>/dev/null | \
			grep -q true; then
			return 0
		fi
		i=$((i + 1))
		sleep 1
	done
	return 1
}

# clean slate: kill leftovers, drop stale stored nodes, restart controller
killall openmeshd 2>/dev/null || true
sleep 1
rm -rf "$AGENT_DIR"
while uci -q delete openmesh.@node[0]; do :; done
uci commit openmesh
/etc/init.d/openmeshd restart
wait_ubus || fail "controller did not come up"

echo "== 1. 发现与领养 =="
openmeshd -d -s "$AGENT_DIR" -P 17805 -D 7689 -A 127.0.0.1 \
	-c 127.0.0.1 -j >/tmp/om-e2e-agent.log 2>&1 &
AGENT_PID=$!
trap 'kill $AGENT_PID 2>/dev/null || true; rm -rf /tmp/om-e2e*' EXIT

i=0
while [ $i -lt 15 ]; do
	ID=$(ubus call openmesh list_pending 2>/dev/null | \
		jsonfilter -e '@.nodes[0].id' 2>/dev/null || true)
	if [ -n "$ID" ]; then
		break
	fi
	i=$((i + 1))
	sleep 1
done
[ -n "$ID" ] || fail "agent not discovered"

ubus call openmesh adopt "{ \"id\": \"$ID\", \"name\": \"test-agent\" }" \
	>/dev/null || fail "adopt call rejected"
wait_online || fail "node never came online"
echo "   adopted: $ID"

echo "== 2. UCI 持久化 =="
uci show openmesh | grep -q "name='test-agent'" || \
	fail "name not persisted"
[ -s "$AGENT_DIR/network.key" ] || fail "network.key missing"

echo "== 3. 控制器重启 =="
/etc/init.d/openmeshd restart
wait_ubus || fail "controller did not restart"
N=$(ubus call openmesh list_adopted | jsonfilter -e '@.nodes[0].id' | wc -c)
[ "$N" -gt 2 ] || fail "adopted list lost after restart"

echo "== 4. agent 回归(WELCOME, 无 join_open) =="
kill "$AGENT_PID" 2>/dev/null || true
sleep 1
openmeshd -d -s "$AGENT_DIR" -P 17805 -D 7689 -A 127.0.0.1 \
	-c 127.0.0.1 >>/tmp/om-e2e-agent.log 2>&1 &
AGENT_PID=$!
wait_online || fail "no WELCOME reconnection"
grep -q "controller link established" /tmp/om-e2e-agent.log || \
	fail "agent log lacks establishment"

kill "$AGENT_PID" 2>/dev/null || true
echo "E2E-OK: 发现→领养→加密会话→UCI持久化→重启保留→WELCOME回归"
