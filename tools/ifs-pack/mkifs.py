#!/usr/bin/env python3
"""Pack Linux (zImage + DTB) into an image the stock Boom! Box IPL boots,
and validate such images offline.

    mkifs.py pack --shim shim.bin --kernel zImage --dtb board.dtb -o hogtied.ifs
    mkifs.py verify hogtied.ifs [more images...]

Layout (IPL behavior per docs/findings/CROSS_CHECKS.md sec. 8):

  region 1 "startup"  [0, startup_size)            sums to 0 mod 2^32
      0x000  QNX startup header (0x100 bytes)
      0x100  shim (startup_vaddr = image_paddr + 0x100)
      ...    last word: checksum filler
  region 2 "payload"  [startup_size, stored_size)  sums to 0 mod 2^32
      +0x000 payload table (PT_*), then zImage and DTB, 4 KiB aligned
      ...    last word: checksum filler

The IPL reads stored_size bytes to 0x84000000, verifies both sums, copies
region 1 to image_paddr, sets header+0x28 = 0x84000000 + startup_size and
jumps to startup_vaddr. The shim finds the payload table through +0x28.

Never writes to hardware. Writing an image to NAND is a separate, manual
step that must go through stock update_nand_teb (PROJECT_DECISIONS.md).
"""
import argparse
import struct
import sys

SIGNATURE = 0x00FF7EEB
HDR_SIZE = 0x100
EM_ARM = 40
IMAGE_PADDR = 0x80100000          # same as stock
IPL_LOAD_ADDR = 0x84000000        # where the IPL reads the whole image
MAX_STORED = 0x2A00000            # IPL limit: one 42 MiB slot
DRAM_END = 0x90000000             # 256 MiB, correct on both RAM variants

PT_MAGIC = 0x584C5448             # "HTLX"
PT_VERSION = 2
PT_FMT = '<8I'                    # magic, version, kernel_off, kernel_size,
PT_SIZE = 0x40                    # dtb_off, dtb_size, mem_cell_off, reserved
ZIMAGE_MAGIC = 0x016F2818         # at zImage + 0x24
FDT_MAGIC = 0xD00DFEED            # big-endian at DTB + 0

# DRAM size. The DTB ships with 256 MiB (right on every unit). The shim reads
# EMIF4 SDRAM_CONFIG ROWSIZE exactly like stock startup (0x80100A18) and, on
# a 512 MiB unit, rewrites the memory node's size cell, whose offset within
# the DTB is mem_cell_off.
MEM_NODE = 'memory@80000000'
MEM_BASE, MEM_SIZE_DEFAULT = 0x80000000, 0x10000000


def fdt_mem_size_cell(dtb):
    """Offset within the DTB of the size cell of /memory@80000000's reg.

    Minimal flattened-device-tree walk (tokens BEGIN_NODE=1, END_NODE=2,
    PROP=3, NOP=4, END=9). Requires reg = <base size> with one cell each
    and base 0x80000000 / size 256 MiB, so the shim's patch is well-defined.
    """
    off_struct, off_strings = struct.unpack_from('>II', dtb, 8)
    p, path = off_struct, []
    while True:
        tok = struct.unpack_from('>I', dtb, p)[0]
        p += 4
        if tok == 1:
            end = dtb.index(b'\0', p)
            path.append(dtb[p:end].decode())
            p = align(end + 1, 4)
        elif tok == 2:
            path.pop()
        elif tok == 3:
            ln, nameoff = struct.unpack_from('>II', dtb, p)
            p += 8
            name = dtb[off_strings + nameoff:dtb.index(b'\0', off_strings + nameoff)].decode()
            if path == ['', MEM_NODE] and name == 'reg':
                if ln != 8:
                    sys.exit(f'/{MEM_NODE} reg must be one address + one size cell')
                base, size = struct.unpack_from('>II', dtb, p)
                if (base, size) != (MEM_BASE, MEM_SIZE_DEFAULT):
                    sys.exit(f'/{MEM_NODE} reg is {base:#x}+{size:#x}, expected 256 MiB at 0x80000000')
                return p + 4
            p = align(p + ln, 4)
        elif tok == 4:
            continue
        elif tok == 9:
            sys.exit(f'no /{MEM_NODE} reg property in the DTB')
        else:
            sys.exit(f'malformed DTB token {tok:#x}')


def align(n, a):
    return (n + a - 1) // a * a


def word_sum(data):
    assert len(data) % 4 == 0
    return sum(struct.unpack(f'<{len(data) // 4}I', data)) & 0xFFFFFFFF


def seal(region, size_align=4):
    """Pad so the sealed length is a multiple of size_align, then end with
    the filler word that makes the region sum to zero."""
    region = bytearray(region)
    region += b'\0' * (align(len(region) + 4, size_align) - 4 - len(region))
    region += struct.pack('<I', (-word_sum(bytes(region))) & 0xFFFFFFFF)
    return bytes(region)


def header(startup_size, stored_size):
    h = bytearray(HDR_SIZE)
    struct.pack_into('<IHBBHHIIIIIIIII', h, 0,
                     SIGNATURE,
                     1,                         # version
                     0, 0,                      # flags1/flags2: uncompressed, physical
                     HDR_SIZE, EM_ARM,
                     IMAGE_PADDR + HDR_SIZE,    # startup_vaddr = shim entry
                     0,                         # paddr_bias
                     IMAGE_PADDR,               # image_paddr
                     IMAGE_PADDR,               # ram_paddr (IPL copy destination)
                     startup_size,              # ram_size: only our own footprint
                     startup_size,
                     stored_size,
                     0,                         # imagefs_paddr (IPL fills it in)
                     stored_size - startup_size)
    return h            # 0x40..0xff (info area) left zero for the IPL's slot info


def pack(shim, kernel, dtb):
    if struct.unpack_from('<I', kernel, 0x24)[0] != ZIMAGE_MAGIC:
        sys.exit('kernel is not an ARM zImage (no 0x016f2818 at +0x24)')
    if struct.unpack_from('>I', dtb, 0)[0] != FDT_MAGIC:
        sys.exit('dtb is not a flattened device tree')

    kernel_off = 0x1000
    dtb_off = align(kernel_off + len(kernel), 0x1000)
    mem_cell_off = fdt_mem_size_cell(dtb)
    table = struct.pack(PT_FMT, PT_MAGIC, PT_VERSION, kernel_off, len(kernel),
                        dtb_off, len(dtb), mem_cell_off, 0).ljust(PT_SIZE, b'\0')
    payload = bytearray(table.ljust(kernel_off, b'\0'))
    payload += kernel
    payload += b'\0' * (dtb_off - len(payload))
    payload += dtb
    payload = seal(payload)

    # The payload lands at 0x84000000 + startup_size, so a 4 KiB-multiple
    # startup region keeps the zImage and DTB page-aligned (the ARM boot
    # protocol needs the DTB 8-byte aligned).
    startup_size = len(seal(bytes(HDR_SIZE) + shim, 0x1000))
    stored_size = startup_size + len(payload)
    startup = seal(bytes(header(startup_size, stored_size)) + shim, 0x1000)
    assert len(startup) == startup_size
    return startup + payload


def verify(image, name='image'):
    """Return a list of problems (empty = OK) and print a summary."""
    problems = []
    if len(image) < HDR_SIZE:
        return [f'{name}: shorter than a header']
    (sig, ver, f1, f2, hsz, mach, vaddr, bias, ipaddr, rpaddr, rsize,
     st, stored, ifs_p, ifs_sz) = struct.unpack_from('<IHBBHHIIIIIIIII', image, 0)
    if sig != SIGNATURE:
        problems.append(f'signature {sig:#x} != {SIGNATURE:#x}')
    if hsz != HDR_SIZE or mach != EM_ARM:
        problems.append(f'header_size {hsz:#x} / machine {mach} unexpected')
    if not (HDR_SIZE <= st <= stored <= len(image)):
        problems.append(f'sizes: startup {st:#x}, stored {stored:#x}, file {len(image):#x}')
    if stored - 1 > MAX_STORED - 1:
        problems.append(f'stored_size {stored:#x} exceeds IPL limit {MAX_STORED:#x}')
    if st % 4 or stored % 4:
        problems.append('region sizes must be word multiples')
    if not problems:
        s1, s2 = word_sum(image[:st]), word_sum(image[st:stored])
        if s1:
            problems.append(f'startup region sums to {s1:#010x}, not 0')
        if s2:
            problems.append(f'payload region sums to {s2:#010x}, not 0')
        copy_dest = bias + rpaddr
        if not copy_dest <= vaddr < copy_dest + st:
            problems.append(f'startup_vaddr {vaddr:#x} outside the copied startup region')
    # HogTiedOS payload checks (skipped for stock QNX images)
    hogtied = len(image) >= st + PT_SIZE and struct.unpack_from('<I', image, st)[0] == PT_MAGIC
    if hogtied and not problems:
        _, pver, koff, ksz, doff, dsz, mcell, _ = struct.unpack_from(PT_FMT, image, st)
        plen = stored - st
        if pver != PT_VERSION:
            problems.append(f'payload table version {pver}')
        if not (koff + ksz <= plen and doff + dsz <= plen):
            problems.append('payload table points outside the image')
        elif struct.unpack_from('<I', image, st + koff + 0x24)[0] != ZIMAGE_MAGIC:
            problems.append('no zImage magic at kernel_off')
        elif struct.unpack_from('>I', image, st + doff)[0] != FDT_MAGIC:
            problems.append('no FDT magic at dtb_off')
        elif not (mcell % 4 == 0 and 0 < mcell < dsz and
                  struct.unpack_from('>I', image, st + doff + mcell)[0] == MEM_SIZE_DEFAULT):
            problems.append('mem_cell_off does not point at a 256 MiB size cell in the DTB')
        if (IPL_LOAD_ADDR + st + doff) % 8:
            problems.append('DTB would not be 8-byte aligned in RAM')
        if IPL_LOAD_ADDR + stored > DRAM_END:
            problems.append('image does not fit in 256 MiB of DRAM at 0x84000000')
    kind = 'HogTiedOS' if hogtied else 'non-HogTiedOS (e.g. stock QNX)'
    print(f'{name}: {kind}, startup {st:#x}, stored {stored:#x} '
          f'({stored / 2**20:.2f} MiB), entry {vaddr:#010x}: '
          + ('OK' if not problems else 'FAIL'))
    for p in problems:
        print(f'  - {p}')
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('pack')
    p.add_argument('--shim', required=True)
    p.add_argument('--kernel', required=True)
    p.add_argument('--dtb', required=True)
    p.add_argument('-o', '--output', required=True)
    v = sub.add_parser('verify')
    v.add_argument('images', nargs='+')
    a = ap.parse_args()
    if a.cmd == 'pack':
        img = pack(open(a.shim, 'rb').read(), open(a.kernel, 'rb').read(),
                   open(a.dtb, 'rb').read())
        if verify(img, a.output):
            sys.exit('refusing to write an image that fails verification')
        open(a.output, 'wb').write(img)
    else:
        bad = [f for f in a.images if verify(open(f, 'rb').read(), f)]
        sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
