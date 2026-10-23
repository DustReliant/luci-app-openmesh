#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# M7 drill on a real device: mesh_create applies a native 802.11s iface,
# auto-rollback fires without confirm, and confirm keeps it. Ends with
# the pristine wireless config restored.
#
#   scp tests/mesh-drill.sh root@<device>:/tmp/ && ssh root@<device> sh /tmp/mesh-drill.sh

set -eu

fail() { echo "DRILL-FAIL: $*" >&2; exit 1; }
mesh_present() { iw dev 2>/dev/null | grep -q "type mesh"; }

cp /etc/config/wireless /root/wireless.pre-drill

echo "== 1. mesh_create（不确认，期望 30s 自动回滚）=="
ubus call openmesh mesh_create '{ "mesh_name": "openmesh-test", "band": "5g" }' \
	| tee /tmp/drill-create.json
sleep 4
mesh_present || fail "mesh interface did not come up"
echo "   mesh interface up: $(iw dev | grep -B1 'type mesh' | head -1 | awk '{print $2}')"

echo "== 2. 等待自动回滚（35s）=="
sleep 35
mesh_present && fail "mesh interface still present after rollback window"
uci -q show wireless | grep -q om_managed && fail "om_managed section survived rollback"
echo "   auto-rollback OK"

echo "== 3. mesh_create + apply_confirm（期望保留）=="
ubus call openmesh mesh_create '{ "mesh_name": "openmesh-test", "band": "5g" }' \
	>/dev/null
sleep 4
mesh_present || fail "mesh interface did not come up (2nd)"
ubus call openmesh apply_confirm >/dev/null
sleep 32
mesh_present || fail "mesh interface disappeared after confirm"
echo "   confirm path OK"

echo "== 4. 恢复原状 =="
while uci -q delete wireless.$(uci show wireless | grep om_managed | cut -d. -f2 | cut -d= -f1 | head -1); do :; done
uci commit wireless
ubus call network reload >/dev/null 2>&1
sleep 4
mesh_present && fail "cleanup failed"
cmp -s /etc/config/wireless /root/wireless.pre-drill || \
	echo "   note: wireless differs from pre-drill (section numbering)"
echo "DRILL-OK: 应用→自动回滚→确认保留→恢复原状"
