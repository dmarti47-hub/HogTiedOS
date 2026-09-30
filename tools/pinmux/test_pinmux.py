#!/usr/bin/env python3
"""The compiled board DTB must program exactly the stock IPL's pad table:
the same 155 (address, value) pairs, no more, no fewer.

    HOGTIED_DTB=output/images/hogtied-boombox.dtb \
        python3 -m unittest tools/pinmux/test_pinmux.py -v
"""
import os
import struct
import sys
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, HERE)
import gen_pinmux as G  # noqa: E402

IPL = os.path.join(HERE, '..', '..', 'firmware/iso/extracted/usr/share/IFS/'
                   'ipl-hbas-dra526-hdisys-nand.bin')
DTB = os.environ.get('HOGTIED_DTB', '')


def fdt_nodes(dtb):
    """{path: {prop: bytes}} for every node in a flattened device tree."""
    off_struct, off_strings = struct.unpack_from('>II', dtb, 8)
    nodes, path, p = {}, [], off_struct
    while True:
        tok = struct.unpack_from('>I', dtb, p)[0]
        p += 4
        if tok == 1:
            end = dtb.index(b'\0', p)
            path.append(dtb[p:end].decode())
            nodes['/'.join(path)] = {}           # root is ''
            p = (end + 4) & ~3
        elif tok == 2:
            path.pop()
        elif tok == 3:
            ln, nameoff = struct.unpack_from('>II', dtb, p)
            name = dtb[off_strings + nameoff:dtb.index(b'\0', off_strings + nameoff)].decode()
            nodes['/'.join(path)][name] = dtb[p + 8:p + 8 + ln]
            p = (p + 8 + ln + 3) & ~3
        elif tok == 4:
            continue
        elif tok == 9:
            return nodes


@unittest.skipUnless(os.path.exists(IPL) and os.path.exists(DTB),
                     'needs stock IPL under firmware/ and HOGTIED_DTB')
class DtbMatchesIplPadTable(unittest.TestCase):
    def test_same_pads_same_values(self):
        nodes = fdt_nodes(open(DTB, 'rb').read())
        programmed = {}
        for label, base, _, _ in G.CONTROLLERS:
            short = label[len('omap3_pmx_'):]
            hits = [p for p in nodes if p.endswith(f'/board-{short}-pins')]
            self.assertEqual(len(hits), 1, f'board-{short}-pins node')
            parent = nodes[hits[0].rsplit('/', 1)[0]]
            self.assertEqual(parent.get('status'), b'okay\0', f'{label} not enabled')
            cells = struct.unpack(f'>{len(nodes[hits[0]]["pinctrl-single,pins"]) // 4}I',
                                  nodes[hits[0]]['pinctrl-single,pins'])
            for off, val in zip(cells[::2], cells[1::2]):
                addr = base + off
                self.assertNotIn(addr, programmed, f'{addr:#x} programmed twice')
                programmed[addr] = val
        expected = dict(G.read_table(IPL))
        self.assertEqual(programmed, expected)


if __name__ == '__main__':
    unittest.main()
