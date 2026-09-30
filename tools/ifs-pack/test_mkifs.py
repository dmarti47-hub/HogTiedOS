#!/usr/bin/env python3
"""End-to-end offline test: a packed HogTiedOS image must be accepted by the
*stock* IPL (run in tools/ipl-emu) and reach the zImage with the ARM Linux
boot registers. Needs firmware/ (stock IPL) and a built shim, zImage and DTB:

    HOGTIED_SHIM=... HOGTIED_ZIMAGE=... HOGTIED_DTB=... \\
        python3 -m unittest tools/ifs-pack/test_mkifs.py -v
"""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'ipl-emu'))
import ipl_emu as E  # noqa: E402
import mkifs  # noqa: E402

IPL = os.path.join(HERE, '..', '..', 'firmware/iso/extracted/usr/share/IFS/'
                   'ipl-hbas-dra526-hdisys-nand.bin')
INPUTS = [os.environ.get(k, '') for k in ('HOGTIED_SHIM', 'HOGTIED_ZIMAGE', 'HOGTIED_DTB')]


# CONTROL_STATUS[14:13] package strap -> ROWSIZE the stock IPL programs
# (0x40200AA4: default 0x280 = ROWSIZE 5; any strap bit set -> 0x200 = 4).
STRAP_512M = 0x0000
STRAP_256M = 0x6000


def boot(image, slot=0, control_status=STRAP_256M):
    nand = E.Nand()
    nand.load_image(image, 4 + slot * E.IFS_BLOCKS)
    emu = E.IplEmu(IPL, nand, E.Eeprom(E.default_eeprom()),
                   control_status=control_status)
    emu.follow = True
    emu.run(400_000_000)
    return emu


@unittest.skipUnless(os.path.exists(IPL) and all(map(os.path.exists, INPUTS)),
                     'needs stock IPL under firmware/ and HOGTIED_SHIM/ZIMAGE/DTB')
class PackedImageBootsOnStockIpl(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        shim, kernel, dtb = (open(p, 'rb').read() for p in INPUTS)
        cls.image = mkifs.pack(shim, kernel, dtb)
        cls.kernel, cls.dtb = kernel, dtb

    def test_validator_accepts(self):
        self.assertEqual(mkifs.verify(self.image, 'packed'), [])

    def test_stock_ipl_reaches_zimage_with_boot_registers(self):
        emu = boot(self.image)
        self.assertIsNotNone(emu.handoff, 'stock IPL rejected the image')
        self.assertEqual(emu.handoff['pc'], mkifs.IMAGE_PADDR + mkifs.HDR_SIZE)
        k = emu.kernel_entry
        self.assertIsNotNone(k, 'shim never entered the kernel')
        r0, r1, r2, _ = k['regs']
        self.assertEqual((r0, r1), (0, 0xFFFFFFFF))
        mem = emu.u
        # pc is the first byte of the exact zImage we packed
        self.assertEqual(bytes(mem.mem_read(k['pc'], 0x40)), self.kernel[:0x40])
        # r2 points at the exact DTB we packed
        self.assertEqual(bytes(mem.mem_read(r2, len(self.dtb))), self.dtb)
        self.assertEqual(r2 % 8, 0)
        self.assertEqual(k['sctlr'] & 0x1005, 0)     # MMU and caches still off
        self.assertIn('HogTiedOS shim: starting Linux', emu.console.decode('latin-1'))

    def _dtb_at_entry(self, emu):
        r2 = emu.kernel_entry['regs'][2]
        return bytes(emu.u.mem_read(r2, len(self.dtb)))

    def _rowsize(self, emu):
        cfg = struct.unpack('<I', emu.u.mem_read(E.EMIF4_SDRAM_CONFIG, 4))[0]
        return (cfg >> 7) & 7

    def test_256m_unit_keeps_256m(self):
        emu = boot(self.image, control_status=STRAP_256M)
        self.assertEqual(self._rowsize(emu), 4)        # set by the stock IPL
        self.assertEqual(self._dtb_at_entry(emu), self.dtb)
        self.assertNotIn('512 MiB', emu.console.decode('latin-1'))

    def test_512m_unit_gets_512m_in_dtb(self):
        emu = boot(self.image, control_status=STRAP_512M)
        self.assertEqual(self._rowsize(emu), 5)        # set by the stock IPL
        cell = mkifs.fdt_mem_size_cell(self.dtb)
        expected = bytearray(self.dtb)
        expected[cell:cell + 4] = struct.pack('>I', 0x20000000)
        self.assertEqual(self._dtb_at_entry(emu), bytes(expected))  # only that cell
        self.assertIn('512 MiB', emu.console.decode('latin-1'))

    def test_shim_writes_only_uart3(self):
        for strap in (STRAP_256M, STRAP_512M):
            emu = boot(self.image, control_status=strap)
            self.assertIsNotNone(emu.kernel_entry)
            others = [(hex(pc), hex(a)) for pc, a, _ in emu.post_handoff_writes
                      if a != E.UART3]
            self.assertEqual(others, [], 'shim wrote to hardware other than UART3')

    def test_boots_from_slot_2(self):
        self.assertIsNotNone(boot(self.image, slot=2).kernel_entry)

    def test_corrupted_payload_rejected_by_ipl(self):
        bad = bytearray(self.image)
        bad[len(bad) // 2] ^= 0x80
        self.assertTrue(mkifs.verify(bytes(bad), 'corrupted'))
        self.assertIsNone(boot(bytes(bad)).handoff)


if __name__ == '__main__':
    unittest.main()
