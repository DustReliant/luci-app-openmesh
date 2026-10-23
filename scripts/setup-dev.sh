#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Symlink openmeshd and luci-app-openmesh into an OpenWrt build tree.
#
# Usage: ./scripts/setup-dev.sh /path/to/openwrt

set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OWRT=${1:?Usage: setup-dev.sh /path/to/openwrt}

[ -d "$OWRT/package" ] || {
	echo "error: $OWRT does not look like an OpenWrt tree (no package/)" >&2
	exit 1
}

for pkg in openmeshd luci-app-openmesh; do
	ln -sfn "$REPO/$pkg" "$OWRT/package/$pkg"
	echo "linked: $OWRT/package/$pkg -> $REPO/$pkg"
done

echo "next: $OWRT make package/openmeshd/compile package/luci-app-openmesh/compile"
