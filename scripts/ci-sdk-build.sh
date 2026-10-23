#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# CI helper: build both packages against a downloaded official OpenWrt SDK.
# The SDK tarball name is discovered from the download directory index, so
# patch levels are never hardcoded.
#
# Usage: scripts/ci-sdk-build.sh <ver> <target-dir> <repo-dir>
#   ver:        23.05 | 24.10 | master
#   target-dir: x86/64 | mediatek/filogic

set -eu

VER=${1:?ver}
TARGET=${2:?target}
REPO=${3:?repo-dir}

DL=https://downloads.openwrt.org

case "$VER" in
master)
	BASE=$DL/snapshots
	;;
*)
	LATEST=$(curl -sfL "$DL/releases/" \
		| grep -oE "$VER\.[0-9]+" | sort -uV | tail -1)
	[ -n "$LATEST" ] || { echo "error: no $VER release found" >&2; exit 1; }
	BASE=$DL/releases/$LATEST
	;;
esac

SDK_FILE=$(curl -sfL "$BASE/targets/$TARGET/" \
	| grep -oE 'openwrt-sdk-[^"]*Linux-x86_64\.tar\.(xz|zst)' \
	| sort -uV | tail -1)
if [ -z "$SDK_FILE" ]; then
	# downloads.openwrt.org directory indexes intermittently 502;
	# the sha256sums file is served fine and lists every artifact
	SDK_FILE=$(curl -sfL "$BASE/targets/$TARGET/sha256sums" \
		| grep -oE 'openwrt-sdk-[^ ]*Linux-x86_64\.tar\.(xz|zst)' \
		| sort -uV | tail -1)
fi
[ -n "$SDK_FILE" ] || {
	echo "error: SDK not found under $BASE/targets/$TARGET/" >&2
	exit 1
}

echo "== $VER ($BASE) target $TARGET"
echo "== SDK: $SDK_FILE"

SDK_DIR=/tmp/sdk
rm -rf "$SDK_DIR"
mkdir -p "$SDK_DIR"
curl -sfL -o "$SDK_DIR/sdk.tar" "$BASE/targets/$TARGET/$SDK_FILE"
tar -xaf "$SDK_DIR/sdk.tar" -C "$SDK_DIR" --strip-components=1
rm -f "$SDK_DIR/sdk.tar"
cd "$SDK_DIR"

# git.openwrt.org is unreliable from CI runners (and some dev networks);
# the GitHub mirrors of the official feeds are faster and dependable.
sed -i \
	-e 's#https://git.openwrt.org/openwrt/openwrt.git#https://github.com/openwrt/openwrt.git#' \
	-e 's#https://git.openwrt.org/feed/packages.git#https://github.com/openwrt/packages.git#' \
	-e 's#https://git.openwrt.org/project/luci.git#https://github.com/openwrt/luci.git#' \
	-e 's#https://git.openwrt.org/feed/routing.git#https://github.com/openwrt/routing.git#' \
	-e 's#https://git.openwrt.org/feed/telephony.git#https://github.com/openwrt/telephony.git#' \
	feeds.conf.default

./scripts/feeds update -a
./scripts/feeds install -a

ln -s "$REPO/openmeshd" package/openmeshd
ln -s "$REPO/luci-app-openmesh" package/luci-app-openmesh

printf 'CONFIG_PACKAGE_openmeshd=y\nCONFIG_PACKAGE_luci-app-openmesh=y\n' >> .config
make defconfig

make -j"$(nproc)" package/openmeshd/compile package/luci-app-openmesh/compile V=w

"$REPO/tests/smoke.sh" "$SDK_DIR"
