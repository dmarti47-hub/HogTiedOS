# Offline maps and navigation (plan)

Chosen approach: like stock, the radio carries its own maps and computes
routes itself (no phone needed). Stock uses NNG iGO (QNX binaries, licensed
map data in `/fs/mmc0/nav` on the eMMC, a gyroscope over IOC IPC channel 5
for dead reckoning); none of it can be reused, so HogTiedOS uses open data
and open software:

- **Map data:** OpenStreetMap extracts from Geofabrik (ODbL; credit
  "© OpenStreetMap contributors" on the map page).
- **Engine:** [libosmscout](https://github.com/Framstag/libosmscout): map
  database, AGG renderer (draws into a memory buffer the UI shows), routing
  and turn instructions. Built for phones and small devices.

## Map building (PC)

`tools/maps/build_map.sh NAME GEOFABRIK_PATH` downloads an extract, checks
its md5, converts it with libosmscout's Import and keeps only the runtime
files (listed in the map's `db.json`). Needs libosmscout built in
`build/osmscout-pc` (see below) and these packages:
`libagg-dev libfreetype-dev libprotobuf-dev protobuf-compiler libmarisa-dev`
(plus nlohmann_json, fetched into `build/prefix`).

Measured (2026-10-01): Wisconsin, 280 MB download → **332 MB map**, 3 min
20 s to convert; a 400×240 frame renders in ~0.1 s on the PC (the radio's
Cortex-A8 is expected to be 10-20× slower: to be measured).

## Stages

1. Map pipeline (done for one state).
2. Moving-map page: riding style (dark, bold roads, big names, little
   clutter), centred on GPS, zoom, north-up / heading-up, drawn off the UI
   thread.
3. Routing: destination from saved places, then address search; route line.
4. Turn-by-turn: next-turn panel, distance, rerouting. Voice once the DSP
   audio link exists.
5. On the radio: libosmscout + AGG + FreeType in the image, maps in
   `hogtied/maps/` on the eMMC next to (never replacing) Harley's, speed
   tuning.

Open: free space on the eMMC (needs the unit); which regions to ship.
