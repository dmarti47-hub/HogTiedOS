#!/bin/sh
# Buildroot post-image step: wrap zImage + DTB into hogtied.ifs, an image the
# stock Boom! Box IPL boots, and validate it. The build fails if validation
# fails. Nothing here writes to hardware.
set -eu

BOARD_DIR="$(dirname "$0")"
TOOLS="${BR2_EXTERNAL_HOGTIED_PATH}/../tools/ifs-pack"
CROSS="${HOST_DIR}/bin/arm-linux-"
WORK="${BUILD_DIR}/hogtied-ifs"

# Every shared library a program needs must exist in the image (a missing
# liblvgl.so once made hogtied-ui unable to start; only a hardware boot
# would have shown it).
missing=0
for f in "${TARGET_DIR}"/usr/bin/* "${TARGET_DIR}"/usr/sbin/* "${TARGET_DIR}"/bin/* "${TARGET_DIR}"/sbin/*; do
    [ -f "$f" ] && [ ! -L "$f" ] || continue
    for lib in $("${HOST_DIR}/bin/arm-linux-readelf" -d "$f" 2>/dev/null |
                 sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        if [ ! -e "${TARGET_DIR}/lib/${lib}" ] && [ ! -e "${TARGET_DIR}/usr/lib/${lib}" ]; then
            echo "post-image: ${f#${TARGET_DIR}} needs missing ${lib}" >&2
            missing=1
        fi
    done
done
[ "$missing" = 0 ] || exit 1

mkdir -p "${WORK}"
"${CROSS}gcc" -c -march=armv7-a -marm -o "${WORK}/shim.o" "${TOOLS}/shim.S"
if "${CROSS}readelf" -r "${WORK}/shim.o" | grep -q 'Relocation section'; then
    echo "shim.S must be position-independent (has relocations)" >&2
    exit 1
fi
"${CROSS}objcopy" -O binary -j .text "${WORK}/shim.o" "${WORK}/shim.bin"

python3 "${TOOLS}/mkifs.py" pack \
    --shim "${WORK}/shim.bin" \
    --kernel "${BINARIES_DIR}/zImage" \
    --dtb "${BINARIES_DIR}/hogtied-boombox.dtb" \
    -o "${BINARIES_DIR}/hogtied.ifs"
python3 "${TOOLS}/mkifs.py" verify "${BINARIES_DIR}/hogtied.ifs"
