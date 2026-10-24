# SPDX-License-Identifier: GPL-3.0-or-later
#
# Copyright (C) 2026 DustReliant <https://github.com/DustReliant>
#
# This is free software, licensed under the GNU General Public License v3.0
# or later. See LICENSE for details.

include $(TOPDIR)/rules.mk

PKG_NAME:=openmeshd
PKG_VERSION:=0.0.1
PKG_RELEASE:=16
PKG_LICENSE:=GPL-3.0-or-later
PKG_MAINTAINER:=DustReliant <https://github.com/DustReliant>

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/openmeshd
  SECTION:=net
  CATEGORY:=Network
  TITLE:=OpenMesh Manager daemon
  DEPENDS:=+libubox +libubus +libblobmsg-json +libsodium +libuci
endef

define Package/openmeshd/description
  Orchestration and management daemon for OpenMesh Manager. Discovers,
  adopts and monitors OpenWrt mesh nodes on top of native 802.11s /
  batman-adv data plane. This M0 build is a skeleton (uloop + heartbeat).
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) \
		-Wall -Wextra -std=c11 \
		-o $(PKG_BUILD_DIR)/openmeshd $(PKG_BUILD_DIR)/openmeshd.c \
		$(PKG_BUILD_DIR)/identity.c \
		$(PKG_BUILD_DIR)/discovery.c \
		$(PKG_BUILD_DIR)/config.c \
		$(PKG_BUILD_DIR)/ubus_api.c \
		$(PKG_BUILD_DIR)/session.c \
		$(PKG_BUILD_DIR)/session_net.c \
		$(PKG_BUILD_DIR)/store.c \
		$(PKG_BUILD_DIR)/netconf.c \
		$(PKG_BUILD_DIR)/monitor.c \
		$(TARGET_LDFLAGS) -lubox -lubus -lblobmsg_json -lsodium -luci
endef

define Package/openmeshd/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/openmeshd $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/etc/config/openmesh $(1)/etc/config/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/etc/init.d/openmeshd $(1)/etc/init.d/
endef

define Package/openmeshd/conffiles
/etc/config/openmesh
endef

$(eval $(call BuildPackage,openmeshd))
