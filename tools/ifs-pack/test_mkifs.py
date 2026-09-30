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


def boot(image, slot=0):
    nand = E.Nand()
    nand.load_image(image, 4 + slot * E.IFS_BLOCKS)
    emu = E.IplEmu(IPL, nand, E.Eeprom(E.default_eeprom()))
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

    def test_boots_from_slot_2(self):
        self.assertIsNotNone(boot(self.image, slot=2).kernel_entry)

    def test_corrupted_payload_rejected_by_ipl(self):
        bad = bytearray(self.image)
        bad[len(bad) // 2] ^= 0x80
        self.assertTrue(mkifs.verify(bytes(bad), 'corrupted'))
        self.assertIsNone(boot(bytes(bad)).handoff)


if __name__ == '__main__':
    unittest.main()
