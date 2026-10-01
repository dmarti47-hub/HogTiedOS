#!/bin/sh
# Build and run the HogTiedOS screen in a desktop window on a Linux PC.
#
#   software/run-ui-on-pc.sh            # demo ride (as an OE FLTR)
#   software/run-ui-on-pc.sh --bike 9   # demo ride as a 2-speaker Tri Glide
#   software/run-ui-on-pc.sh --can vcan0
#   software/run-ui-on-pc.sh --replay my-ride.log
#
# Phone music: pair your phone with this PC as usual (its own Bluetooth
# settings); the Media page then shows the track and its buttons control the
# phone. hbas-btd is started alongside (only if libdbus-1-dev is installed).
#
# GPS page: hbas-gpsd plays a made-up demo ride (software/gpsd/demo/ride.nmea),
# since a PC has no u-blox receiver.
#
# Map page: shown when libosmscout is installed in build/prefix and a map
# has been built with tools/maps/build_map.sh (docs/NAVIGATION.md); it
# follows the demo ride. Up/Down zoom, Enter: north-up / heading-up.
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

# map page if libosmscout and a map are there
MAP_ARGS=""
MAP_CMAKE="-DHOGTIED_MAP=OFF"
MAP_DIR=$(dirname "$(ls "$ROOT"/build/maps/*/db.json 2>/dev/null | head -1)" 2>/dev/null || true)
MAP_FONT=$(fc-match -f '%{file}' "DejaVu Sans" 2>/dev/null || true)
if [ -f "$ROOT/build/prefix/lib/cmake/libosmscout/libosmscoutConfig.cmake" ] &&
   [ -n "$MAP_DIR" ] && [ "$MAP_DIR" != "." ] && [ -n "$MAP_FONT" ]; then
    MAP_CMAKE="-DHOGTIED_MAP=ON -DCMAKE_PREFIX_PATH=$ROOT/build/prefix"
    MAP_ARGS="--map-dir $MAP_DIR --map-style $ROOT/software/maps/hogtied.oss --map-font $MAP_FONT"
    echo "Map page: $MAP_DIR"
else
    echo "(Map page: no map built; see docs/NAVIGATION.md)"
fi

cmake -S "$ROOT/software/hogtied-ui" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
      -DLVGL_DIR="$LVGL_DIR" -DHOGTIED_SDL=ON $MAP_CMAKE >/dev/null
cmake --build "$BUILD" -j"$(nproc)"

# Bluetooth media daemon: optional on a PC (needs libdbus-1-dev)
BTD=""
if pkg-config --exists dbus-1; then
    cmake -S "$ROOT/software/btd" -B "$ROOT/build/btd-pc" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$ROOT/build/btd-pc" -j"$(nproc)"
    BTD="$ROOT/build/btd-pc/hbas-btd"
else
    echo "(Media page: install libdbus-1-dev to control your phone's music from here)"
fi

# GPS daemon, replaying the demo ride
cmake -S "$ROOT/software/gpsd" -B "$ROOT/build/gpsd-pc" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/build/gpsd-pc" -j"$(nproc)"
GPSD="$ROOT/build/gpsd-pc/hbas-gpsd"

# no data source given: play the demo ride (as an OE FLTR unless --bike is set)
case " $* " in
*" --demo "*|*" --replay "*|*" --can "*) ;;
*) case " $* " in *" --bike "*) set -- --demo "$@" ;; *) set -- --demo --bike 2 "$@" ;; esac ;;
esac
RUN="${XDG_RUNTIME_DIR:-/tmp}"
pids=""
if [ -n "$BTD" ]; then
    # no --agent: the PC's own Bluetooth settings handle pairing
    "$BTD" --socket "$RUN/hbas-bt.sock" &
    pids="$pids $!"
fi
"$GPSD" --replay "$ROOT/software/gpsd/demo/ride.nmea" --socket "$RUN/hbas-gps.sock" &
pids="$pids $!"
trap 'kill $pids 2>/dev/null' EXIT INT TERM
echo "Starting hogtied-ui $*  (Left/Right: pages, Esc: back, close window to quit)"
# shellcheck disable=SC2086 # MAP_ARGS is a list of options
"$BUILD/hogtied-ui" --bt-socket "$RUN/hbas-bt.sock" --gps-socket "$RUN/hbas-gps.sock" $MAP_ARGS "$@"
