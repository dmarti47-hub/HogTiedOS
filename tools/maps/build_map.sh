#!/bin/sh
# Build an offline map for HogTiedOS from an OpenStreetMap extract.
#
#   tools/maps/build_map.sh NAME GEOFABRIK_PATH
#   tools/maps/build_map.sh wisconsin north-america/us/wisconsin
#
# Downloads https://download.geofabrik.de/<GEOFABRIK_PATH>-latest.osm.pbf,
# checks its md5, converts it with libosmscout's Import (built in
# build/osmscout-pc), and keeps only the files the finished map uses (those
# listed in its db.json), in build/maps/NAME. Only the content HogTiedOS's
# minimal map uses is imported (make_types.py). Map data (c) OpenStreetMap
# contributors, ODbL.
set -eu

[ $# -eq 2 ] || { sed -n '2,12p' "$0"; exit 2; }
NAME=$1
SRC=$2
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMPORT="$ROOT/build/osmscout-pc/Import/Import"
TYPES_DIR="$ROOT/build/maps/types"
TYPES="$TYPES_DIR/map.ost"
OUT="$ROOT/build/maps"
PBF="$OUT/$NAME-latest.osm.pbf"
URL="https://download.geofabrik.de/$SRC-latest.osm.pbf"

[ -x "$IMPORT" ] || { echo "build libosmscout first (build/osmscout-pc)"; exit 1; }
# our minimal content selection (tools/maps/make_types.py)
python3 "$ROOT/tools/maps/make_types.py" "$ROOT/build/libosmscout/stylesheets" "$TYPES_DIR" >/dev/null
mkdir -p "$OUT"
if [ ! -f "$PBF" ]; then
    echo "Downloading $URL"
    curl -fSL -o "$PBF.part" "$URL"
    curl -fsSL -o "$PBF.md5" "$URL.md5"
    mv "$PBF.part" "$PBF"
fi
(cd "$OUT" && sed "s|  .*$|  $NAME-latest.osm.pbf|" "$PBF.md5" | md5sum -c -)

rm -rf "$OUT/$NAME.tmp"
mkdir -p "$OUT/$NAME.tmp"
echo "Converting (a few minutes per state)..."
"$IMPORT" --typefile "$TYPES" --destinationDirectory "$OUT/$NAME.tmp" --eco true "$PBF" \
    > "$OUT/import-$NAME.log" 2>&1 || { tail -20 "$OUT/import-$NAME.log"; exit 1; }

# keep only what the map uses at runtime (db.json lists it)
python3 - "$OUT/$NAME.tmp" <<'PY'
import json, os, sys
d = sys.argv[1]
keep = set(json.load(open(os.path.join(d, 'db.json')))['output']['files']) | {'db.json'}
for f in os.listdir(d):
    if f not in keep:
        os.remove(os.path.join(d, f))
PY
rm -rf "$OUT/$NAME"
mv "$OUT/$NAME.tmp" "$OUT/$NAME"
echo "Map ready: $OUT/$NAME ($(du -sh "$OUT/$NAME" | cut -f1))"
