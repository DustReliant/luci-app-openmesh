#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build both packages inside an OpenWrt build tree (after setup-dev.sh).
#
# Usage: ./scripts/build.sh /path/to/openwrt [make args...]
#
# Dev notes (M0 findings):
#   - packages must be enabled in .config, otherwise compile silently no-ops:
#       printf 'CONFIG_PACKAGE_openmeshd=y\nCONFIG_PACKAGE_luci-app-openmesh=y\n' >> .config
#       make defconfig
#   - after editing openmeshd/src/*, run "build.sh <tree> package/openmeshd/clean"
#     first: Build/Prepare stamps do not track local source edits.

set -eu

OWRT=${1:?Usage: build.sh /path/to/openwrt [make args...]}
shift

if [ $# -eq 0 ]; then
	set -- package/openmeshd/compile package/luci-app-openmesh/compile
fi

make -C "$OWRT" "$@"
