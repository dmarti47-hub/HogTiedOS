#!/usr/bin/env python3
"""Regression tests: the harness must accept the stock image and reject
corrupted ones, exactly like the real IPL. Needs the stock firmware under
firmware/ (gitignored); skips if it's absent.

    python3 -m unittest tools/ipl-emu/test_ipl_emu.py -v
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import ipl_emu as E  # noqa: E402

ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
IFS_DIR = os.path.join(ROOT, 'firmware/iso/extracted/usr/share/IFS')
IPL = os.path.join(IFS_DIR, 'ipl-hbas-dra526-hdisys-nand.bin')
STOCK = os.path.join(IFS_DIR, 'completeifs-premium.bin')
MAX_INSNS = 400_000_000


def boot(slots, valid=(0, 1, 2)):
    nand = E.Nand()
    for slot, image in slots.items():
        nand.load_image(bytes(image), 4 + slot * E.IFS_BLOCKS)
    ee = E.Eeprom(E.default_eeprom(valid))
    emu = E.IplEmu(IPL, nand, ee)
    emu.run(MAX_INSNS)
    return emu, ee


def flip(image, offset):
    bad = bytearray(image)
    bad[offset] ^= 1
    return bad


@unittest.skipUnless(os.path.exists(IPL) and os.path.exists(STOCK), 'stock firmware not present')
class StockIplTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stock = open(STOCK, 'rb').read()

    def test_stock_image_hands_off_with_linux_friendly_state(self):
        emu, ee = boot({0: self.stock})
        h = emu.handoff
        self.assertIsNotNone(h)
        self.assertEqual(h['pc'], 0x801004A0)
        self.assertEqual(h['cpsr'] & 0x1F, 0x13)      # SVC mode
        self.assertTrue(h['cpsr'] & 0xC0)             # IRQ+FIQ masked
        self.assertEqual(h['sctlr'] & 0x1005, 0)      # MMU, D-cache, I-cache off
        self.assertEqual(ee.writes, [(E.EE_CURRENT_IFS, 0x1)])

    def test_corrupt_startup_rejected(self):
        emu, _ = boot({0: flip(self.stock, 0x1000)})
        self.assertIsNone(emu.handoff)

    def test_corrupt_imagefs_rejected(self):
        emu, _ = boot({0: flip(self.stock, 0x100000)})
        self.assertIsNone(emu.handoff)

    def test_fallback_to_next_slot_and_invalidate_bad_one(self):
        emu, ee = boot({0: flip(self.stock, 0x1000), 1: self.stock})
        self.assertEqual(emu.handoff['pc'], 0x801004A0)
        self.assertIn((0x0, 0x0), ee.writes)          # slot 0 flag cleared
        self.assertIn((0x4, 0x0), ee.writes)          # ...and its mirror

    def test_invalid_slot_skipped(self):
        emu, ee = boot({0: self.stock, 1: self.stock}, valid=(1, 2))
        self.assertEqual(emu.handoff['pc'], 0x801004A0)
        self.assertEqual(ee.writes[0], (E.EE_CURRENT_IFS, 0x2))   # went straight to slot 1

    def test_all_failed_retries_invalid_slots(self):
        # Fail-open: slot 0 marked invalid, 1/2 empty. After 1 and 2 fail the
        # IPL retries every slot and boots slot 0 anyway.
        emu, _ = boot({0: self.stock}, valid=(1, 2))
        self.assertEqual(emu.handoff['pc'], 0x801004A0)
        self.assertIn('All partitions failed, retrying', emu.console.decode('latin-1'))


if __name__ == '__main__':
    unittest.main()
