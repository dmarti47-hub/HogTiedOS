################################################################################
#
# hogtied-lvgl
#
# Source-only package: downloads and unpacks LVGL for hogtied-ui, which
# builds it with its own lv_conf.h. (Buildroot skips downloads for the
# `local` hogtied-ui package, so the tarball can't be an extra download
# there.)
#
################################################################################

HOGTIED_LVGL_VERSION = 9.2.2
HOGTIED_LVGL_SOURCE = v$(HOGTIED_LVGL_VERSION).tar.gz
HOGTIED_LVGL_SITE = https://github.com/lvgl/lvgl/archive/refs/tags
HOGTIED_LVGL_LICENSE = MIT
HOGTIED_LVGL_LICENSE_FILES = LICENCE.txt
HOGTIED_LVGL_INSTALL_TARGET = NO

$(eval $(generic-package))
