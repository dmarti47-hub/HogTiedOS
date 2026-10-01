#!/usr/bin/env python3
"""Make HogTiedOS's map content selection from libosmscout's type config.

    make_types.py UPSTREAM_STYLESHEETS_DIR OUT_DIR

Copies map.ost and the modules it uses (motorways, max_speeds,
contour_lines) and marks every TYPE that isn't in KEEP as IGNORE, so the
importer leaves it out. The result is a minimal map like Harley's own:
drivable roads (with names, addresses and routing data), water, borders,
place names and fuel. Buildings, land use, paths, shops, transit and the
like make up most of OpenStreetMap and aren't needed.
"""
import os
import re
import sys

KEEP = [
    # roads a motorcycle can use
    r'highway_motorway(_trunk|_primary|_link|_junction)?',
    r'highway_(trunk|primary|secondary|tertiary)(_link)?',
    r'highway_(unclassified|road|residential|living_street|service|services)',
    r'highway_(roundabout|mini_roundabout|turning_cycle)',
    r'route_ferry', r'routemaster_ferry',
    # water
    r'waterway_(river|riverbank|canal|dock)', r'natural_(water|bay|strait)',
    r'landuse_(reservoir|basin)',
    # borders and places
    r'boundary_(country|state|administrative)',
    r'place_(continent|country|state|region|capitalcity|millioncity|halfmillioncity|bigcity'
    r'|city|town|village|hamlet|suburb|locality|island|islet|sea|ocean)',
    # search and the rider's essentials
    r'address', r'amenity_fuel',
    # generic relation types the importer uses for streets, places, tunnels
    r'(street|tunnel|wayparts|place|town|suburb|state)_any',
]
KEEP_RE = re.compile('^(' + '|'.join(KEEP) + ')$')
TYPE_RE = re.compile(r'^(\s*)TYPE\s+(\S+)')
FILES = ['map.ost', 'motorways.ost', 'max_speeds.ost', 'contour_lines.ost']


def filter_ost(text):
    out, kept, dropped = [], [], []
    lines = text.splitlines(keepends=True)
    for i, line in enumerate(lines):
        out.append(line)
        m = TYPE_RE.match(line)
        if not m:
            continue
        name = m.group(2)
        already = re.search(r'\bIGNORE\b', line[m.end():].split('//')[0]) or \
            (i + 1 < len(lines) and lines[i + 1].strip().startswith('IGNORE'))
        if KEEP_RE.match(name):
            kept.append(name)
        else:
            dropped.append(name)
            if not already:
                out.append(m.group(1) + '  IGNORE // HogTiedOS: not in the minimal map\n')
    return ''.join(out), kept, dropped


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    src, dst = argv[1], argv[2]
    os.makedirs(dst, exist_ok=True)
    kept_all = []
    for f in FILES:
        text, kept, dropped = filter_ost(open(os.path.join(src, f), encoding='utf-8').read())
        open(os.path.join(dst, f), 'w', encoding='utf-8').write(text)
        kept_all += kept
        print('%-18s keep %3d  ignore %3d' % (f, len(kept), len(dropped)))
    missing = [k for k in KEEP if not any(re.fullmatch(k, n) for n in kept_all)]
    if missing:
        sys.exit('KEEP patterns that matched nothing (upstream renamed?): %s' % missing)


if __name__ == '__main__':
    main(sys.argv)
