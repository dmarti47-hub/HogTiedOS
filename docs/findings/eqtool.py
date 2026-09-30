#!/usr/bin/env python3
"""
eqtool.py - offline validator / checksum repair / dumper for Harley-Davidson
Boom! Box 6.5GT (firmware 1.22.0.3) EQ profile files (*.bin).

Rules below were recovered from the firmware's own validator (audioCtrlSvc
0x127314) and TLV parser (0x127508) and cross-checked against all 132 factory
profiles (0 violations).  This tool NEVER talks to hardware and NEVER modifies
its input; 'fix' writes a new file.

File layout (all integers little-endian):
  u32 length      = file size - 4 (multiple of 4, > 7)
  u32 checksum    = chosen so that the sum of every u32 AFTER the length word
                    (including this word) is 0 mod 2**32
  then TLV records: u32 tag, u32 length (bytes), data (length is a multiple of 4)

Usage:
  eqtool.py verify FILE...            check structure, checksum, tag lengths
  eqtool.py fix IN OUT                write OUT with corrected checksum word
  eqtool.py dump FILE                 metadata + decoded biquads
"""
import struct, sys

# Exact payload lengths enforced by the firmware parser (other lengths -> error 0xFD)
FIXED_LEN = {0x34: 0x28, 0x0F: 0x14, 0x0E: 0x0C, 0x10: 0xA0, 0x2B: 0x0C, 0x12: 4,
             0x18: 8, 0x3C: 4, 0x42: 0x10, 0x45: 4, 0x4D: 0x50, 0x8E: 0x16C,
             0xC0: 0x14C, 0x90: 0x10, 0xB9: 0x30, 0xBA: 0x3C, 0xBB: 0x54, 0xBC: 0x48}
ANY_LEN = {0x36, 0x35, 0x62, 0xBD, 0xBE}          # accepted with any length
MAX_0x61 = 7                                      # at most 7 per-path records
IGNORED = {0x0B, 0x27, 0x3A, 0x46}                # metadata; parser skips them
Q = float(1 << 23)


def parse(data):
    """Return (problems, records). records = [(tag, payload)]."""
    prob = []
    if len(data) < 12 or len(data) % 4:
        return ["size not a multiple of 4 / too small"], []
    length = struct.unpack_from("<I", data, 0)[0]
    if length + 4 != len(data):
        prob.append("length word %d != file size-4 (%d)" % (length, len(data) - 4))
    if length <= 7:
        prob.append("length word too small")
    words = struct.unpack_from("<%dI" % ((len(data) - 4) // 4), data, 4)
    if sum(words) & 0xFFFFFFFF:
        prob.append("checksum: sum of words after length word = 0x%08x (must be 0)" % (sum(words) & 0xFFFFFFFF))
    recs, o = [], 8
    while o + 8 <= len(data):
        tag, ln = struct.unpack_from("<II", data, o)
        if ln % 4 or o + 8 + ln > len(data):
            prob.append("bad TLV at offset %d (tag 0x%x len %d)" % (o, tag, ln))
            break
        recs.append((tag, data[o + 8:o + 8 + ln]))
        o += 8 + ln
    else:
        if o != len(data):
            prob.append("trailing bytes after last TLV")
    n61 = 0
    for tag, pl in recs:
        if tag in FIXED_LEN and len(pl) != FIXED_LEN[tag]:
            prob.append("tag 0x%02x length %d, firmware requires %d" % (tag, len(pl), FIXED_LEN[tag]))
        if tag == 0x61:
            n61 += 1
            if len(pl) != 0x38:
                prob.append("tag 0x61 record length %d, expected 56" % len(pl))
        if tag == 0x36 and len(pl) % 20:
            prob.append("tag 0x36 length not a multiple of 20")
    if n61 > MAX_0x61:
        prob.append("more than %d tag-0x61 records" % MAX_0x61)
    return prob, recs


def fixed(data):
    """Return data with the checksum word (offset 4) recomputed."""
    b = bytearray(data)
    words = struct.unpack_from("<%dI" % ((len(b) - 8) // 4), b, 8)
    struct.pack_into("<I", b, 4, (-sum(words)) & 0xFFFFFFFF)
    return bytes(b)


def biquads(payload):
    """File record order (strong inference, see notes): A1, A2, B1, B0, B2 as signed Q5.23."""
    for i in range(0, len(payload), 20):
        a1, a2, b1, b0, b2 = (v / Q for v in struct.unpack(">5i", payload[i:i + 20]))
        yield a1, a2, b1, b0, b2


def cmd_verify(paths):
    rc = 0
    for p in paths:
        prob, recs = parse(open(p, "rb").read())
        print("%-45s %s" % (p, "OK" if not prob else "PROBLEMS"))
        for x in prob:
            print("    -", x)
        rc |= bool(prob)
    return rc


def cmd_dump(p):
    data = open(p, "rb").read()
    prob, recs = parse(data)
    print("file:", p, "| problems:", prob or "none")
    for tag, pl in recs:
        if tag == 0x27:
            print("  tuning file name (0x27):", pl.split(b"\0")[0].decode("ascii", "replace"))
        elif tag == 0x46:
            print("  tool version   (0x46):", pl.split(b"\0")[0].decode("ascii", "replace"))
    for tag, pl in recs:
        if tag == 0x36:
            print("  biquads (0x36): %d records  [A1, A2, B1, B0, B2]" % (len(pl) // 20))
            for k, b in enumerate(biquads(pl)):
                print("   %2d  %s" % (k, "  ".join("%+.6f" % v for v in b)))
        elif tag == 0x35:
            print("  filter map (0x35): %d bytes: %s" % (len(pl), pl.hex(" ")))
        elif tag == 0x61:
            pass
    print("  tag-0x61 records:", sum(1 for t, _ in recs if t == 0x61))


def main(argv):
    if len(argv) >= 3 and argv[1] == "verify":
        return cmd_verify(argv[2:])
    if len(argv) == 4 and argv[1] == "fix":
        out = fixed(open(argv[2], "rb").read())
        open(argv[3], "wb").write(out)
        print("wrote", argv[3])
        return cmd_verify([argv[3]])
    if len(argv) == 3 and argv[1] == "dump":
        return cmd_dump(argv[2])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
