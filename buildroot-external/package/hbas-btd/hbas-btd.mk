################################################################################
#
# hbas-btd
#
################################################################################

HBAS_BTD_VERSION = local
HBAS_BTD_SITE = $(BR2_EXTERNAL_HOGTIED_PATH)/../software
HBAS_BTD_SITE_METHOD = local
HBAS_BTD_SUBDIR = btd
HBAS_BTD_LICENSE = MIT
HBAS_BTD_DEPENDENCIES = dbus host-pkgconf

define HBAS_BTD_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(HBAS_BTD_PKGDIR)/S39bt-restore \
		$(TARGET_DIR)/etc/init.d/S39bt-restore
	$(INSTALL) -D -m 0755 $(HBAS_BTD_PKGDIR)/S45bluealsa \
		$(TARGET_DIR)/etc/init.d/S45bluealsa
	$(INSTALL) -D -m 0755 $(HBAS_BTD_PKGDIR)/S85hbas-btd \
		$(TARGET_DIR)/etc/init.d/S85hbas-btd
	$(INSTALL) -D -m 0755 $(HBAS_BTD_PKGDIR)/hbas-bt-save \
		$(TARGET_DIR)/usr/sbin/hbas-bt-save
endef

$(eval $(cmake-package))
