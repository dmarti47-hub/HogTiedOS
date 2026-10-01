"""Checks for the Harley EQ preset generator. Needs the stock firmware
(gitignored firmware/); skipped without it."""
import filecmp
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
ACS = os.path.join(ROOT, 'firmware/stock_ifs/secondary/files/bin/audioCtrlSvc')
EQ = os.path.join(ROOT, 'firmware/iso/extracted/mnt/persistence/eq')
TABLE = os.path.join(ROOT, 'software/libhbas/src/harley_eq_table.c')
sys.path.insert(0, HERE)


@unittest.skipUnless(os.path.exists(ACS) and os.path.isdir(EQ), 'stock firmware not extracted')
class HarleyTone(unittest.TestCase):
    def test_knob_8_is_flat_and_tone_slots_follow_knobs(self):
        import gen_harley_eq as g
        from harley_tone import ToneDesigner
        d = ToneDesigner(ACS)
        recs = g.tone_records(os.path.join(EQ, '02_ON.bin'))
        flat = d.design(recs, bass=8, treble=8)
        loud = d.design(recs, bass=16, treble=16)
        self.assertAlmostEqual(g.biquad_db(flat[1], 60), 0.0, places=1)    # bass slot
        self.assertAlmostEqual(g.biquad_db(flat[2], 10000), 0.0, places=1) # treble slot
        self.assertGreater(g.biquad_db(loud[1], 60), 1.5)
        self.assertGreater(g.biquad_db(loud[2], 10000), 8)

    def test_committed_table_is_reproducible(self):
        import gen_harley_eq as g
        with tempfile.TemporaryDirectory() as t:
            out = os.path.join(t, 'table.c')
            g.main(['gen', ACS, EQ, out])
            self.assertTrue(filecmp.cmp(out, TABLE, shallow=False),
                            'harley_eq_table.c differs from a fresh generation')


if __name__ == '__main__':
    unittest.main()
