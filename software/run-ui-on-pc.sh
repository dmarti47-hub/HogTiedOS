#!/bin/sh
# Build and run the HogTiedOS screen in a desktop window on a Linux PC.
#
#   software/run-ui-on-pc.sh            # demo ride (as an OE FLTR)
#   software/run-ui-on-pc.sh --bike 9   # demo ride as a 2-speaker Tri Glide
#   software/run-ui-on-pc.sh --can vcan0
#   software/run-ui-on-pc.sh --replay my-ride.log
#
# Keys: Left/Right change pages, Up/Down, Enter selects, Esc goes back.
# Close the window or press Ctrl+C in the terminal to quit.
# Nothing here touches the head unit.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build/ui-pc"
LVGL_VER=9.2.2
LVGL_SHA256=129b4e00e06639fa79d7e8a6cab3c1ecce2445b1a246652ccd34f22e7b17ad6f
LVGL_DIR="$ROOT/build/lvgl-$LVGL_VER"

missing=""
for tool in cc cmake make pkg-config curl tar sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
if [ -z "$missing" ] && ! pkg-config --exists sdl2; then
    missing="$missing libsdl2-dev"
fi
if [ -n "$missing" ]; then
    echo "Missing:$missing"
    echo "On Ubuntu/Debian/Pop!_OS, install them with:"
    echo "  sudo apt install build-essential cmake pkg-config curl libsdl2-dev"
    exit 1
fi

if [ ! -f "$LVGL_DIR/lvgl.h" ]; then
    echo "Downloading LVGL $LVGL_VER..."
    mkdir -p "$ROOT/build"
    tarball="$ROOT/build/lvgl-$LVGL_VER.tar.gz"
    curl -fL -o "$tarball" "https://github.com/lvgl/lvgl/archive/refs/tags/v$LVGL_VER.tar.gz"
    echo "$LVGL_SHA256  $tarball" | sha256sum -c -
    tar -xzf "$tarball" -C "$ROOT/build"
fi

cmake -S "$ROOT/software/hogtied-ui" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
      -DLVGL_DIR="$LVGL_DIR" -DHOGTIED_SDL=ON >/dev/null
cmake --build "$BUILD" -j"$(nproc)"

# no data source given: play the demo ride (as an OE FLTR unless --bike is set)
case " $* " in
*" --demo "*|*" --replay "*|*" --can "*) ;;
*) case " $* " in *" --bike "*) set -- --demo "$@" ;; *) set -- --demo --bike 2 "$@" ;; esac ;;
esac
echo "Starting hogtied-ui $*  (Left/Right: pages, Esc: back, close window to quit)"
exec "$BUILD/hogtied-ui" "$@"
