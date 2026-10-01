################################################################################
#
# hogtied-ui
#
################################################################################

HOGTIED_UI_VERSION = local
HOGTIED_UI_SITE = $(BR2_EXTERNAL_HOGTIED_PATH)/../software
HOGTIED_UI_SITE_METHOD = local
HOGTIED_UI_SUBDIR = hogtied-ui
HOGTIED_UI_LICENSE = MIT
HOGTIED_UI_DEPENDENCIES = hogtied-lvgl

HOGTIED_UI_CONF_OPTS = \
	-DLVGL_DIR=$(HOGTIED_LVGL_DIR) \
	-DHOGTIED_FBDEV=ON

define HOGTIED_UI_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(HOGTIED_UI_PKGDIR)/S90hogtied-ui \
		$(TARGET_DIR)/etc/init.d/S90hogtied-ui
	$(INSTALL) -D -m 0755 $(HOGTIED_UI_PKGDIR)/S30emmc \
		$(TARGET_DIR)/etc/init.d/S30emmc
endef

$(eval $(cmake-package))
