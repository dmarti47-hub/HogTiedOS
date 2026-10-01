################################################################################
#
# hbas-gpsd
#
################################################################################

HBAS_GPSD_VERSION = local
HBAS_GPSD_SITE = $(BR2_EXTERNAL_HOGTIED_PATH)/../software
HBAS_GPSD_SITE_METHOD = local
HBAS_GPSD_SUBDIR = gpsd
HBAS_GPSD_LICENSE = MIT

define HBAS_GPSD_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(HBAS_GPSD_PKGDIR)/S35hbas-gpsd \
		$(TARGET_DIR)/etc/init.d/S35hbas-gpsd
endef

$(eval $(cmake-package))
