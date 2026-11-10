#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# om-evidence.sh -- device-side transaction evidence collector (m22-2,
# Phase 7.2 slice 2). Captures pre/post config + runtime state with
# hashes so a test round can prove: expected changes only, and full
# restoration after rollback.
#
# Usage: om-evidence.sh <tag>        (e.g. om-evidence.sh pre-apply)
# Output: /tmp/om-evidence-<tag>.tar.gz + sha256 list on stdout

set -eu

TAG="${1:?usage: om-evidence.sh <tag>}"
OUT="/tmp/om-ev-$TAG"
mkdir -p "$OUT"

# ---- config files + hashes ----
for f in network wireless openmesh dhcp; do
	if [ -f "/etc/config/$f" ]; then
		cp "/etc/config/$f" "$OUT/config-$f"
	fi
done
( cd "$OUT" && sha256sum config-* 2>/dev/null > files.sha256 ) || true

# ---- runtime state ----
ip -br link > "$OUT/ip-br-link.txt" 2>&1 || true
ip addr > "$OUT/ip-addr.txt" 2>&1 || true
ip route > "$OUT/ip-route.txt" 2>&1 || true
ip -4 route show default >> "$OUT/ip-route.txt" 2>&1 || true
if command -v bridge >/dev/null; then
	bridge link > "$OUT/bridge-link.txt" 2>&1 || true
fi
ubus call network.device status > "$OUT/ubus-netdev.json" 2>&1 || true

# ---- BATMAN state ----
if command -v batctl >/dev/null; then
	batctl meshif om_bat interface > "$OUT/batctl-if.txt" 2>&1 || true
	batctl meshif om_bat originators > "$OUT/batctl-o.txt" 2>&1 || true
	batctl meshif om_bat transglobal > "$OUT/batctl-tg.txt" 2>&1 || true
fi

# ---- OpenMesh view ----
ubus call openmesh topology > "$OUT/om-topology.json" 2>&1 || true
ubus call openmesh get_mesh_info > "$OUT/om-mesh-info.json" 2>&1 || true
ubus call openmesh ports > "$OUT/om-ports.json" 2>&1 || true
ubus call openmesh get_links > "$OUT/om-links.json" 2>&1 || true

# ---- management reachability marker ----
{
	echo "collected: $(date)"
	echo "hostname: $(uci -q get system.@system[0].hostname || cat /proc/sys/kernel/hostname)"
	echo "board: $(cat /tmp/sysinfo/board_name 2>/dev/null)"
	echo "uptime: $(cut -d. -f1 /proc/uptime)"
} > "$OUT/meta.txt"

tar -czf "/tmp/om-evidence-$TAG.tar.gz" -C /tmp "om-ev-$TAG" >/dev/null 2>&1
rm -rf "$OUT"
sha256sum "/tmp/om-evidence-$TAG.tar.gz"
echo "evidence: /tmp/om-evidence-$TAG.tar.gz"
