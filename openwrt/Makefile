include $(TOPDIR)/rules.mk

PKG_NAME:=tsched
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_INSTALL:=1

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/tsched
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Lightweight task scheduler
  DEPENDS:=+libc
endef

define Package/tsched/conffiles
/etc/tsched/tsched.conf
/etc/tsched/tasks.conf
endef

define Package/tsched-web
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Web UI for tsched
  DEPENDS:=+tsched +uhttpd
endef

define Package/tsched/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/bin $(1)/etc/tsched $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/tschedd $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/tschedctl $(1)/usr/bin/
	$(INSTALL_CONF) ./files/tsched.conf $(1)/etc/tsched/
	$(INSTALL_CONF) ./files/tasks.conf $(1)/etc/tsched/
	$(INSTALL_BIN) ./files/tschedd.init $(1)/etc/init.d/tschedd
endef

define Package/tsched-web/install
	$(INSTALL_DIR) $(1)/www/tsched $(1)/www/cgi-bin
	$(CP) $(PKG_INSTALL_DIR)/usr/share/tsched/www/* $(1)/www/tsched/
	$(LN) /usr/bin/tschedctl $(1)/www/cgi-bin/tsched-api
endef

$(eval $(call BuildPackage,tsched))
$(eval $(call BuildPackage,tsched-web))
