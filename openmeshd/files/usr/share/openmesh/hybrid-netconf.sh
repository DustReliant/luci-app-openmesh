#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Mode B (hybrid) network orchestration for OpenMesh Manager (m19).
# Executed by openmeshd netconf as: sh /usr/share/openmesh/hybrid-netconf.sh
#
# Env contract:
#   OM_LAN_PORTS  unset/"" = no wired leg | all = every br-lan physical
#                 port | "lan1,lan2" = named ports become batman hardifs
#   OM_NET_SYS    network sysfs root (default /sys/class/net; test hook)
#
# Idempotent and migration-safe: stale om_eth_* hardifs from a previous
# selection are re-homed into br-lan before the new selection applies.
# Expected misses (absent ports/options) never fail the run; uci commit
# plus an om_bat sanity check decide the exit status. A missing br-lan
# bridge is a hard error: without it hybrid cannot bridge bat0 into lan
# and the caller must roll the transaction back.

NET_SYS="${OM_NET_SYS:-/sys/class/net}"

BR=$(uci -q show network 2>/dev/null |
	sed -n "s/^network\.\([^.]*\)\.name='br-lan'$/\1/p" | head -n 1)
[ -n "$BR" ] || exit 1

# batman soft interface + wifi hardif carrier
uci -q set network.om_bat=interface
uci -q set network.om_bat.proto='batadv'
uci -q set network.om_bat.om_managed='1'
uci -q set network.om_mesh=interface
uci -q set network.om_mesh.proto='batadv_hardif'
uci -q set network.om_mesh.master='om_bat'
uci -q set network.om_mesh.om_managed='1'

# bring batman into the lan bridge (once)
uci -q get network.$BR.ports 2>/dev/null |
	grep -q om_bat || uci -q add_list network.$BR.ports='om_bat'

# re-home stale wired hardifs from a previous selection
for S in $(uci -q show network 2>/dev/null |
	sed -n "s/^network\.\(om_eth_[A-Za-z0-9_]*\)=interface$/\1/p"); do
	DEV=$(uci -q get network.$S.device 2>/dev/null)
	if [ -n "$DEV" ]; then
		uci -q add_list network.$BR.ports="$DEV"
	fi
	uci -q delete network.$S
done

# operator-forbidden ports (openmesh.main.protected_ports): enforced
# HERE because every path into the wired leg funnels through this
# script -- a protected port (e.g. the NAS uplink) is never touched,
# neither when named explicitly nor inside an "all" expansion.
PROT=$(uci -q get openmesh.main.protected_ports 2>/dev/null)
is_protected() {
	case " $PROT " in
	*" $1 "*) return 0 ;;
	esac
	return 1
}

# resolve the wired-leg selection
SEL=""
if [ "${OM_LAN_PORTS:-}" = "all" ]; then
	SEL=$(uci -q get network.$BR.ports 2>/dev/null)
elif [ -n "${OM_LAN_PORTS:-}" ]; then
	SEL=$(echo "$OM_LAN_PORTS" | tr ',' ' ')
fi
for P in $SEL; do
	case "$P" in
	om_*|br-*|lo) continue ;;
	esac
	[ -d "$NET_SYS/$P" ] || continue
	is_protected "$P" && continue
	uci -q del_list network.$BR.ports="$P"
	uci -q set network.om_eth_$P=interface
	uci -q set network.om_eth_$P.proto='batadv_hardif'
	uci -q set network.om_eth_$P.master='om_bat'
	uci -q set network.om_eth_$P.device="$P"
	uci -q set network.om_eth_$P.om_managed='1'
done

uci -q commit network || exit 1
[ "$(uci -q get network.om_bat.proto 2>/dev/null)" = "batadv" ] || exit 1
exit 0
