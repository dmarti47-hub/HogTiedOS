################################################################################
#
# hbas-iocd
#
################################################################################

HBAS_IOCD_VERSION = local
HBAS_IOCD_SITE = $(BR2_EXTERNAL_HOGTIED_PATH)/../software
HBAS_IOCD_SITE_METHOD = local
HBAS_IOCD_SUBDIR = iocd
HBAS_IOCD_LICENSE = MIT

define HBAS_IOCD_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(HBAS_IOCD_PKGDIR)/S80hbas-iocd \
		$(TARGET_DIR)/etc/init.d/S80hbas-iocd
endef

$(eval $(cmake-package))
