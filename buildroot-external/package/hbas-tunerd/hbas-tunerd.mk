################################################################################
#
# hbas-tunerd
#
################################################################################

HBAS_TUNERD_VERSION = local
HBAS_TUNERD_SITE = $(BR2_EXTERNAL_HOGTIED_PATH)/../software
HBAS_TUNERD_SITE_METHOD = local
HBAS_TUNERD_SUBDIR = tunerd
HBAS_TUNERD_LICENSE = MIT

define HBAS_TUNERD_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(HBAS_TUNERD_PKGDIR)/S36hbas-tunerd \
		$(TARGET_DIR)/etc/init.d/S36hbas-tunerd
endef

$(eval $(cmake-package))
