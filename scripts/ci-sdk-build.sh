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
	# the downloads.openwrt.org releases/ index 502s from runner IPs;
	# prefer GitHub tags (openwrt mirrors every release tag there)
	LATEST=$(git ls-remote --tags --refs \
		https://github.com/openwrt/openwrt.git "v$VER.*" 2>/dev/null \
		| awk -F/ '{print $NF}' | sed 's/^v//' | sort -uV | tail -1)
	[ -n "$LATEST" ] || LATEST=$(curl -sfL "$DL/releases/" 2>/dev/null \
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

# large feed clones can partially fail with exit 0 on runners; verify
feeds_complete() {
	[ -d package/feeds/packages/libsodium ] || return 1
	[ -d package/feeds/base/libubox ] || return 1
	[ -d package/feeds/luci/luci-base ] || return 1
	return 0
}
./scripts/feeds update -a || ./scripts/feeds update -a
./scripts/feeds install -a || ./scripts/feeds install -a
feeds_complete || {
	echo "feeds incomplete, retrying both steps" >&2
	./scripts/feeds update -a && ./scripts/feeds install -a
}
feeds_complete || { echo "error: feeds still incomplete" >&2; exit 1; }

ln -s "$REPO/openmeshd" package/openmeshd
# luci app is copied, not symlinked: luci.mk's po discovery relies on
# make $(wildcard) which does not traverse symlinked package dirs
rm -rf package/luci-app-openmesh
cp -a "$REPO/luci-app-openmesh" package/luci-app-openmesh

printf 'CONFIG_PACKAGE_openmeshd=y\nCONFIG_PACKAGE_luci-app-openmesh=y\n' >> .config
make defconfig

make -j"$(nproc)" package/openmeshd/compile package/luci-app-openmesh/compile V=w

"$REPO/tests/smoke.sh" "$SDK_DIR"
