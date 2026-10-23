#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Post-build smoke assertions: both ipks exist and the daemon binary stays
# within its flash budget (docs/architecture.md section 3).
#
# OpenWrt ipks are an outer tar.gz holding data.tar.gz/control.tar.gz
# (scripts/ipkg-build); ar-based ipks are handled as a fallback.
#
# Usage: tests/smoke.sh <buildroot-or-sdk-root>

set -eu

ROOT=${1:?Usage: smoke.sh <build-root>}
BIN_MAX=153600   # 150 KiB for the openmeshd binary

fail() { echo "SMOKE-FAIL: $*" >&2; exit 1; }

[ -d "$ROOT/bin/packages" ] || fail "no bin/packages under $ROOT"

# list <ipk>  -> tar-like listing of the data payload
list_ipk() {
	if tar -tzf "$1" >/dev/null 2>&1; then
		tar -xzOf "$1" ./data.tar.gz | tar -tzvf -
	else
		local member
		member=$(ar t "$1" | grep -E '^data\.tar\.(xz|zst|gz)$' | head -1)
		[ -n "$member" ] || fail "no data tar member in $1"
		ar p "$1" "$member" | tar -tvaf -
	fi
}

for name in openmeshd luci-app-openmesh; do
	find "$ROOT/bin/packages" -name "${name}_*.ipk" -o -name "${name}-*.ipk" \
		| grep -q . || fail "$name ipk not found under $ROOT/bin/packages"
done

IPK=$(find "$ROOT/bin/packages" -name 'openmeshd_*.ipk' | head -1)
SIZE=$(list_ipk "$IPK" | awk '$6 == "usr/sbin/openmeshd" || $6 == "./usr/sbin/openmeshd" { print $3; exit }')
[ -n "$SIZE" ] || fail "usr/sbin/openmeshd not found inside $IPK"
[ "$SIZE" -le "$BIN_MAX" ] || fail "openmeshd ${SIZE}B exceeds budget ${BIN_MAX}B"

echo "SMOKE-OK: both ipks present, openmeshd=${SIZE}B (<= ${BIN_MAX}B)"
