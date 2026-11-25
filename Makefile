# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-sacn-monitor
PKG_VERSION:=0.2.1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-sacn-monitor
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=SACNMON passive sACN monitor
  DEPENDS:=+libpcap +libubox +libubus +luci-base +rpcd
endef

define Package/luci-app-sacn-monitor/conffiles
/etc/config/sacnmon
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/* ./LICENSE $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=gnu11 \
		$(TARGET_LDFLAGS) -o $(PKG_BUILD_DIR)/sacnmond \
		$(PKG_BUILD_DIR)/sacnmond.c $(PKG_BUILD_DIR)/sacn.c -lpcap -lubus -lubox
endef

define Package/luci-app-sacn-monitor/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/www
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/sacnmond $(1)/usr/sbin/
	$(CP) ./root/* $(1)/
	$(CP) ./htdocs/* $(1)/www/
endef

define Package/luci-app-sacn-monitor/postinst
#!/bin/sh
[ -z "$${IPKG_INSTROOT}" ] || exit 0
rm -f /tmp/luci-indexcache /tmp/luci-indexcache.*
/etc/init.d/rpcd reload
endef

define Package/luci-app-sacn-monitor/postrm
#!/bin/sh
[ -z "$${IPKG_INSTROOT}" ] || exit 0
rm -f /tmp/luci-indexcache /tmp/luci-indexcache.*
/etc/init.d/rpcd reload
endef

$(eval $(call BuildPackage,luci-app-sacn-monitor))
