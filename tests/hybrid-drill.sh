#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# M9 disconnect-safe drill (run with nohup ON the device; mode B edits
# br-lan and may cut the SSH/LAN session briefly - the daemon's own 30s
# auto-rollback restores connectivity regardless).
#
#   scp tests/hybrid-drill.sh root@<device>:/tmp/
#   ssh root@<device> 'nohup sh /tmp/hybrid-drill.sh >/root/hybrid.log 2>&1 &'

set -u
LOG=/root/hybrid.log
fail() { echo "DRILL-FAIL: $*" >> "$LOG"; exit 1; }
log() { echo "$*" >> "$LOG"; }

has_om_bat() { ip link show om_bat >/dev/null 2>&1; }
mesh_enslaved() {
	batctl meshif om_bat interface 2>/dev/null | grep -q mesh
}
bat_in_lan() {
	ip link show master br-lan 2>/dev/null | grep -q om_bat
}

cp /etc/config/wireless /root/hybrid-w.pre
cp /etc/config/network /root/hybrid-n.pre

log "== 1. mesh_create hybrid =="
ubus call openmesh mesh_create \
	'{ "mesh_name": "openmesh-test", "band": "5g", "mode": "hybrid" }' \
	>> "$LOG" 2>&1
sleep 5
has_om_bat || fail "om_bat (batadv softif) missing"
mesh_enslaved || fail "mesh iface not enslaved to om_bat"
bat_in_lan || log "   note: om_bat not yet in br-lan (netifd timing)"
log "   om_bat up + mesh enslaved"

log "== 2. auto-rollback window (35s) =="
sleep 35
has_om_bat && fail "om_bat survived rollback"
uci -q show network | grep -q "om_bat\|om_mesh" && fail "om network sections survived"
uci -q show wireless | grep -q om_managed && fail "om wireless section survived"
log "   rollback clean"

log "== 3. create + confirm =="
ubus call openmesh mesh_create \
	'{ "mesh_name": "openmesh-test", "band": "5g", "mode": "hybrid" }' \
	>/dev/null 2>&1
sleep 5
ubus call openmesh apply_confirm >/dev/null 2>&1
sleep 33
has_om_bat || fail "om_bat gone after confirm"
log "   confirmed and stable"

log "== 4. cleanup =="
while uci -q delete wireless.$(uci show wireless | grep om_managed | \
	cut -d. -f2 | cut -d= -f1 | head -1); do :; done
uci -q delete network.om_bat
uci -q delete network.om_mesh
BR=$(uci -q show network | sed -n "s/network\.\([^.]*\)\.name='br-lan'/\1/p")
[ -n "$BR" ] && uci -q del_list network.$BR.ports='om_bat'
uci commit wireless
uci commit network
ubus call network reload >/dev/null 2>&1
sleep 5
has_om_bat && fail "cleanup incomplete"
log "DRILL-OK: hybrid 应用→自动回滚→确认保留→复原"
