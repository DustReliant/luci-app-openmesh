#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Link openmeshd and install luci-app-openmesh into an OpenWrt build tree.
# openmeshd: symlink (no po/, proven fine). luci-app-openmesh: copied,
# because luci.mk's po/* discovery uses make $(wildcard) which does not
# reliably traverse symlinked package dirs (i18n packs silently vanish).
#
# Usage: ./scripts/setup-dev.sh /path/to/openwrt

set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OWRT=${1:?Usage: setup-dev.sh /path/to/openwrt}

[ -d "$OWRT/package" ] || {
	echo "error: $OWRT does not look like an OpenWrt tree (no package/)" >&2
	exit 1
}

ln -sfn "$REPO/openmeshd" "$OWRT/package/openmeshd"
echo "linked: $OWRT/package/openmeshd -> $REPO/openmeshd"

rm -rf "$OWRT/package/luci-app-openmesh"
cp -a "$REPO/luci-app-openmesh" "$OWRT/package/luci-app-openmesh"
echo "copied: $REPO/luci-app-openmesh -> $OWRT/package/luci-app-openmesh"

echo "next: $OWRT make package/openmeshd/compile package/luci-app-openmesh/compile"
echo "note: re-run this script after editing luci-app-openmesh/ sources"
