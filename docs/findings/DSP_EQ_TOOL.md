# eqtool.py

Offline validator / checksum-repair / dumper for the Boom! Box 6.5GT EQ profile
format, derived from the firmware's own validator and TLV parser (see
dsp_layout_map.json for the reverse-engineering detail). Tested against all 132
factory EQ profiles with zero violations. Never modifies input files, never
touches hardware.

```
python3 eqtool.py verify FILE...     # check structure + checksum + tag lengths
python3 eqtool.py fix IN OUT         # write OUT with corrected checksum word
python3 eqtool.py dump FILE          # metadata + decoded biquads
```

See the module docstring in eqtool.py for the full file-format writeup.
