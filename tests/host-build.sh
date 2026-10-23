#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build and run host unit tests.
#
# Uses system libsodium headers if present (CI installs libsodium-dev);
# otherwise builds a static libsodium from the OpenWrt download cache or
# upstream into tests/.build (no root required).

set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=$REPO/tests/.build
SRC=$REPO/openmeshd/src
SODIUM_VER=1.0.19
CFLAGS_extra=""
LDLIBS_extra="-lsodium"

mkdir -p "$BUILD"

if [ ! -f /usr/include/sodium.h ]; then
	TARBALL=""
	for d in "$HOME"/Build_OpenWrt/openwrt/dl /usr/src; do
		[ -f "$d/libsodium-$SODIUM_VER.tar.gz" ] && TARBALL="$d/libsodium-$SODIUM_VER.tar.gz"
	done
	if [ -z "$TARBALL" ]; then
		curl -sfL -o "$BUILD/libsodium-$SODIUM_VER.tar.gz" \
			"https://download.libsodium.org/libsodium/releases/libsodium-$SODIUM_VER.tar.gz"
		TARBALL="$BUILD/libsodium-$SODIUM_VER.tar.gz"
	fi

	if [ ! -f "$BUILD/sodium/lib/libsodium.a" ]; then
		rm -rf "$BUILD/libsodium-src"
		mkdir -p "$BUILD/libsodium-src"
		# release tarballs use varying top-level names; strip it
		tar -xzf "$TARBALL" -C "$BUILD/libsodium-src" --strip-components=1
		(cd "$BUILD/libsodium-src" && \
			./configure --prefix="$BUILD/sodium" \
				--disable-shared --enable-static >/dev/null && \
			make -j"$(nproc)" install >/dev/null)
	fi
	CFLAGS_extra="-I$BUILD/sodium/include"
	LDLIBS_extra="$BUILD/sodium/lib/libsodium.a"
fi

# test_discovery needs a helper to look nodes up by hex id
cat > "$BUILD/find_node_hex.h" <<'EOF'
#include <string.h>
static const struct om_node_info *find_node_hex(struct om_disc *d, const char *hex)
{
	for (unsigned i = 0; i < d->n_nodes; i++) {
		char h[OM_ID_HEX_LEN];
		om_id_to_hex(d->nodes[i].id, h);
		if (strcmp(h, hex) == 0)
			return &d->nodes[i];
	}
	return NULL;
}
EOF

# shellcheck disable=SC2086
cc $CFLAGS_extra -I"$SRC" -I"$BUILD" -Wall -Wextra -std=c11 \
	-o "$BUILD/test_identity" \
	"$REPO/tests/test_identity.c" "$SRC/identity.c" \
	$LDLIBS_extra
"$BUILD/test_identity"

# shellcheck disable=SC2086
cc $CFLAGS_extra -I"$SRC" -I"$BUILD" -Wall -Wextra -std=c11 \
	-o "$BUILD/test_discovery" \
	"$REPO/tests/test_discovery.c" "$SRC/identity.c" "$SRC/discovery.c" \
	$LDLIBS_extra
"$BUILD/test_discovery"
