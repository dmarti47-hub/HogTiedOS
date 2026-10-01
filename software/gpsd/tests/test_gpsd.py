#!/usr/bin/env python3
"""hbas-gpsd against a pretend receiver on a pseudo-terminal.

    python3 test_gpsd.py PATH/TO/hbas-gpsd
"""
import os
import select
import socket
import subprocess
import sys
import tempfile
import time
import unittest

GPSD = sys.argv.pop(1) if len(sys.argv) > 1 else None


def sentence(body):
    c = 0
    for ch in body:
        c ^= ord(ch)
    return ('$%s*%02X\r\n' % (body, c)).encode()


EPOCH = [
    sentence('GNRMC,201530.50,A,4228.8640,N,08337.4710,W,30.0,270.0,011026,,,A'),
    sentence('GNGGA,201530.50,4228.8640,N,08337.4710,W,1,08,0.9,250.5,M,-34.0,M,,'),
    sentence('GNGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1'),
    sentence('GPGSV,1,1,03,01,40,083,46,02,17,308,41,12,07,344,'),
]


class GpsdTest(unittest.TestCase):
    def setUp(self):
        self.master, slave = os.openpty()
        self.slave_name = os.ttyname(slave)
        os.close(slave)
        self.tmp = tempfile.mkdtemp()
        self.sock = os.path.join(self.tmp, 'gps.sock')
        self.proc = subprocess.Popen([GPSD, '--device', self.slave_name, '--socket', self.sock],
                                     stderr=subprocess.PIPE)
        for _ in range(100):
            if os.path.exists(self.sock):
                break
            time.sleep(0.05)

    def tearDown(self):
        self.proc.terminate()
        self.proc.wait(5)
        os.close(self.master)

    def read_master(self, timeout=2):
        data = b''
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.master], [], [], 0.1)
            if r:
                data += os.read(self.master, 256)
                if b'\n' in data:
                    break
        return data

    def lines(self, timeout=4):
        s = socket.socket(socket.AF_UNIX)
        s.connect(self.sock)
        s.settimeout(timeout)
        f = s.makefile('r')
        return s, f

    def send_epochs(self, n, gap=0.2, mangle=False):
        for _ in range(n):
            for i, line in enumerate(EPOCH):
                if mangle and i == 0:
                    line = line.replace(b'A,4228', b'A,4229')    # checksum now wrong
                os.write(self.master, line)
            time.sleep(gap)

    def test_1_sends_stocks_baud_switch_first(self):
        self.assertEqual(self.read_master(), b'$PUBX,41,1,0007,0003,57600,0*2B\r\n')

    def test_2_locks_and_reports_the_fix(self):
        self.read_master()
        self.send_epochs(4)
        s, f = self.lines()
        line = ''
        for _ in range(5):
            line = f.readline()
            if 'link=1' in line:
                break
        self.assertIn('link=1', line)
        self.assertIn('valid=1', line)
        self.assertIn('fix=3d', line)
        self.assertIn('lat=42.481067', line)
        self.assertIn('lon=-83.624517', line)
        self.assertIn('used=8', line)
        self.assertIn('time=1790885730', line)
        self.assertIn('sats="1:46 2:41 12:-1"', line)
        s.close()

    def test_3_bad_checksums_are_not_used(self):
        self.read_master()
        self.send_epochs(4, mangle=True)
        s, f = self.lines()
        line = f.readline()
        # the RMC (speed, course, date) had a bad checksum: never applied;
        # GGA's position is fine and does come through
        self.assertIn('valid=0', line)
        self.assertIn('speed=0.0', line)
        self.assertIn('time=0 ', line)
        self.assertIn('lat=42.481067', line)
        s.close()

    def test_4_quiet_receiver_drops_the_link(self):
        self.read_master()
        self.send_epochs(3)
        time.sleep(6)                                  # > 5 s of silence
        s, f = self.lines()
        self.assertIn('link=0', f.readline())
        s.close()


if __name__ == '__main__':
    if not GPSD:
        sys.exit(__doc__)
    unittest.main(argv=[sys.argv[0], '-v'])
