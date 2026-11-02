#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# m19 wired-leg orchestration tests: run the REAL production script
# (openmeshd/files/usr/share/openmesh/hybrid-netconf.sh) against a fake
# uci and a fake /sys/class/net -- same PATH-injection pattern as the
# r21b command-layer tests (tests/test_batman_cmdlayer.sh).
#
# Scenarios:
#   H1 no wired leg      -- om_bat/om_mesh only, br-lan keeps all ports
#   H2 named port        -- lan1 becomes om_eth_lan1, others untouched
#   H3 all ports         -- every physical port becomes a hardif
#   H4 migration lan1->lan2 (re-home: lan1 returns to br-lan)
#   H5 idempotent re-apply of the same selection (state byte-identical)
#   H6 unknown port in list is skipped, known ones still applied
#   H7 hostile OM_LAN_PORTS value: no side effects, no failure
#   H8 missing br-lan bridge: script fails loudly (transaction rolls back)
#   H9 switching wired leg off restores the port into br-lan

set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SCRIPT="$REPO/openmeshd/files/usr/share/openmesh/hybrid-netconf.sh"
T="$REPO/tests/.build/lanports-test"

pass=0
fail=0
ok() { pass=$((pass + 1)); echo "PASS $1"; }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }

[ -f "$SCRIPT" ] || { echo "FATAL: $SCRIPT missing"; exit 1; }

setup() {
	rm -rf "$T"
	mkdir -p "$T/fakebin" "$T/net"
	for p in lan1 lan2 lan3 eth0; do
		mkdir -p "$T/net/$p"
	done
	cat > "$T/state" <<'EOF'
network|device_lan|@type=device
network|device_lan|name|br-lan
network|device_lan|ports|lan1|lan2|lan3
network|lan|@type=interface
network|lan|device|br-lan
network|lan|proto|static
EOF
	cat > "$T/fakebin/uci" <<'EOF'
#!/bin/sh
# fake uci: exactly the surface hybrid-netconf.sh uses. State file
# $UCI_STATE, one record per line:
#   network|<section>|@type=<type>
#   network|<section>|<option>|<value>[|<value>...]
STATE="${UCI_STATE:?UCI_STATE not set}"

while [ "${1#-}" != "$1" ]; do shift; done
cmd="$1"
path="${2:-}"
sec="${path#network.}"
opt=""
val=""
case "$sec" in
*.*) opt="${sec#*.}"; sec="${sec%%.*}" ;;
esac
case "$opt" in
*=*) val="${opt#*=}"; opt="${opt%%=*}" ;;
esac
case "$sec" in
*=*) [ -z "$opt" ] && { val="${sec#*=}"; sec="${sec%%=*}"; } ;;
esac

case "$cmd" in
show)
	awk -F'|' '
	BEGIN { q = sprintf("%c", 39) }
	$1 == "network" {
		if (substr($3, 1, 6) == "@type=") {
			print "network." $2 "=" substr($3, 7)
		} else {
			line = "network." $2 "." $3 "="
			for (i = 4; i <= NF; i++)
				line = line (i > 4 ? " " : "") q $i q
			print line
		}
	}' "$STATE"
	;;
get)
	awk -F'|' -v sec="$sec" -v opt="$opt" '
	$1 == "network" && $2 == sec && $3 == opt {
		line = ""
		for (i = 4; i <= NF; i++)
			line = line (i > 4 ? " " : "") $i
		print line
		found = 1
		exit
	}
	END { exit found ? 0 : 1 }' "$STATE"
	;;
set)
	if [ -n "$opt" ]; then
		awk -F'|' -v sec="$sec" -v opt="$opt" \
			'!($1 == "network" && $2 == sec && $3 == opt)' \
			"$STATE" > "$STATE.new"
		printf '%s|%s|%s|%s\n' network "$sec" "$opt" "$val" >> "$STATE.new"
	else
		awk -F'|' -v sec="$sec" \
			'!($1 == "network" && $2 == sec)' \
			"$STATE" > "$STATE.new"
		printf '%s|%s|@type=%s\n' network "$sec" "$val" >> "$STATE.new"
	fi
	mv "$STATE.new" "$STATE"
	;;
add_list)
	awk -F'|' -v sec="$sec" -v opt="$opt" -v val="$val" '
	$1 == "network" && $2 == sec && $3 == opt {
		print $0 "|" val
		added = 1
		next
	}
	{ print }
	END { if (!added) print "network|" sec "|" opt "|" val }' \
		"$STATE" > "$STATE.new"
	mv "$STATE.new" "$STATE"
	;;
del_list)
	awk -F'|' -v sec="$sec" -v opt="$opt" -v val="$val" '
	$1 == "network" && $2 == sec && $3 == opt {
		out = $1 "|" $2 "|" $3
		nv = 0
		for (i = 4; i <= NF; i++) {
			if ($i == val) continue
			out = out "|" $i
			nv++
		}
		if (nv > 0) print out
		next
	}
	{ print }' "$STATE" > "$STATE.new"
	mv "$STATE.new" "$STATE"
	;;
delete)
	if [ -n "$opt" ]; then
		awk -F'|' -v sec="$sec" -v opt="$opt" \
			'!($1 == "network" && $2 == sec && $3 == opt)' \
			"$STATE" > "$STATE.new"
	else
		awk -F'|' -v sec="$sec" \
			'!($1 == "network" && $2 == sec)' \
			"$STATE" > "$STATE.new"
	fi
	mv "$STATE.new" "$STATE"
	;;
commit)
	exit 0
	;;
*)
	exit 1
	;;
esac
EOF
	chmod +x "$T/fakebin/uci"
}

# run <lan_ports>  ("" = leave OM_LAN_PORTS unset)
run() {
	(
		PATH="$T/fakebin:$PATH"
		UCI_STATE="$T/state"
		OM_NET_SYS="$T/net"
		export PATH UCI_STATE OM_NET_SYS
		if [ -n "$1" ]; then
			OM_LAN_PORTS="$1"
			export OM_LAN_PORTS
		else
			unset OM_LAN_PORTS
		fi
		sh "$SCRIPT"
	)
}

has() { grep -qF "$1" "$T/state"; }
lacks() { ! grep -qF "$1" "$T/state"; }
# bridge membership is a set: re-homing appends, so compare sorted
ports_eq() {
	grep '^network|device_lan|ports|' "$T/state" |
		cut -d'|' -f4- | tr '|' '\n' | sort | tr '\n' '|' | grep -qxF "$1|"
}

echo "== H1: no wired leg =="
setup
if run ""; then ok "H1 rc"; else bad "H1 rc"; fi
has 'network|om_bat|proto|batadv' && ok "H1 om_bat" || bad "H1 om_bat"
has 'network|om_mesh|proto|batadv_hardif' && ok "H1 om_mesh" || bad "H1 om_mesh"
ports_eq 'lan1|lan2|lan3|om_bat' && ok "H1 ports intact" || bad "H1 ports intact"
lacks '|om_eth_' && ok "H1 no om_eth" || bad "H1 no om_eth"

echo "== H2: named port lan1 =="
setup
if run "lan1"; then ok "H2 rc"; else bad "H2 rc"; fi
has 'network|om_eth_lan1|proto|batadv_hardif' && ok "H2 hardif proto" || bad "H2 hardif proto"
has 'network|om_eth_lan1|master|om_bat' && ok "H2 master" || bad "H2 master"
has 'network|om_eth_lan1|device|lan1' && ok "H2 device" || bad "H2 device"
has 'network|om_eth_lan1|om_managed|1' && ok "H2 managed" || bad "H2 managed"
ports_eq 'lan2|lan3|om_bat' && ok "H2 lan1 removed, rest intact" || bad "H2 lan1 removed, rest intact"

echo "== H3: all ports =="
setup
if run "all"; then ok "H3 rc"; else bad "H3 rc"; fi
ports_eq 'om_bat' && ok "H3 only om_bat left in bridge" || bad "H3 only om_bat left in bridge"
has 'network|om_eth_lan1|' && has 'network|om_eth_lan2|' && has 'network|om_eth_lan3|' &&
	ok "H3 three hardifs" || bad "H3 three hardifs"
has 'network|om_eth_wan|' && bad "H3 wan untouched (unexpected section)" || ok "H3 wan untouched"

echo "== H4: migration lan1 -> lan2 =="
setup
run "lan1" >/dev/null 2>&1
if run "lan2"; then ok "H4 rc"; else bad "H4 rc"; fi
lacks 'network|om_eth_lan1|' && ok "H4 old hardif gone" || bad "H4 old hardif gone"
has 'network|om_eth_lan2|device|lan2' && ok "H4 new hardif" || bad "H4 new hardif"
ports_eq 'lan1|lan3|om_bat' && ok "H4 lan1 re-homed" || bad "H4 lan1 re-homed"

echo "== H5: idempotent re-apply =="
setup
run "lan1" >/dev/null 2>&1
cp "$T/state" "$T/state.before"
run "lan1" >/dev/null 2>&1
if cmp -s "$T/state.before" "$T/state"; then ok "H5 state stable"; else bad "H5 state stable"; fi

echo "== H6: unknown port skipped =="
setup
if run "lan9,lan1"; then ok "H6 rc"; else bad "H6 rc"; fi
lacks 'network|om_eth_lan9|' && ok "H6 lan9 ignored" || bad "H6 lan9 ignored"
has 'network|om_eth_lan1|device|lan1' && ok "H6 lan1 still applied" || bad "H6 lan1 still applied"

echo "== H7: hostile selection value =="
setup
CANARY="$T/pwned"
if run "lan1;touch $CANARY"; then ok "H7 rc"; else bad "H7 rc"; fi
[ -e "$CANARY" ] && bad "H7 no canary created" || ok "H7 no canary created"
lacks '|om_eth_' && ok "H7 no hardif from junk" || bad "H7 no hardif from junk"
ports_eq 'lan1|lan2|lan3|om_bat' && ok "H7 ports intact" || bad "H7 ports intact"

echo "== H8: missing br-lan bridge fails loudly =="
setup
	grep -v '^network|device_lan|name|br-lan$' "$T/state" > "$T/state.new"
	mv "$T/state.new" "$T/state"
if run "lan1"; then bad "H8 rc nonzero"; else ok "H8 rc nonzero"; fi

echo "== H9: wired leg switched off restores port =="
setup
run "lan1" >/dev/null 2>&1
if run ""; then ok "H9 rc"; else bad "H9 rc"; fi
lacks '|om_eth_' && ok "H9 hardif removed" || bad "H9 hardif removed"
ports_eq 'lan1|lan2|lan3|om_bat' && ok "H9 port back in bridge" || bad "H9 port back in bridge"

echo
echo "test_hybrid_lanports: $pass PASS, $fail FAIL"
[ "$fail" -eq 0 ] || exit 1
rm -rf "$T"
exit 0
