#!/usr/bin/env python3
"""hbas-btd against a fake BlueZ on a private session bus.

    dbus-run-session -- python3 test_btd.py PATH/TO/hbas-btd

The fake exports what hbas-btd reads from BlueZ (ObjectManager, Adapter1,
Device1, MediaPlayer1, AgentManager1), records the calls it gets, and can
emit signals and call the pairing agent like bluetoothd does.
"""
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

DBusGMainLoop(set_as_default=True)
BTD = sys.argv.pop(1) if len(sys.argv) > 1 else None

ADAPTER = '/org/bluez/hci0'
DEVICE = ADAPTER + '/dev_AA_BB_CC_DD_EE_FF'
PLAYER = DEVICE + '/player0'
PROPS = 'org.freedesktop.DBus.Properties'


class Fake(dbus.service.Object):
    """Root object (ObjectManager) plus AgentManager1 and the per-object
    interfaces, all served from one object with path fallback."""

    def __init__(self, bus):
        self.bus = bus
        self.calls = []
        self.state = {
            ADAPTER: {'org.bluez.Adapter1': {'Powered': True, 'Pairable': False,
                                             'Discoverable': False}},
            DEVICE: {'org.bluez.Device1': {'Alias': "Dave's Pixel", 'Connected': True,
                                           'Paired': True}},
            PLAYER: {'org.bluez.MediaPlayer1': {
                'Status': 'paused', 'Position': dbus.UInt32(1000),
                'Track': dbus.Dictionary({'Title': 'Thunderstruck', 'Artist': 'AC/DC',
                                          'Album': 'The Razors Edge',
                                          'Duration': dbus.UInt32(292000)},
                                         signature='sv')}},
        }
        super().__init__(bus, '/', )
        self._fallback = True

    # ObjectManager
    @dbus.service.method('org.freedesktop.DBus.ObjectManager', out_signature='a{oa{sa{sv}}}')
    def GetManagedObjects(self):
        return self.state

    @dbus.service.signal('org.freedesktop.DBus.ObjectManager', signature='oa{sa{sv}}')
    def InterfacesAdded(self, path, ifaces):
        pass

    @dbus.service.signal('org.freedesktop.DBus.ObjectManager', signature='oas')
    def InterfacesRemoved(self, path, ifaces):
        pass


class Obj(dbus.service.Object):
    def __init__(self, fake, path):
        self.fake = fake
        super().__init__(fake.bus, path)

    @dbus.service.method('org.bluez.MediaPlayer1')
    def Play(self):
        self.fake.calls.append(('Play', self._object_path))

    @dbus.service.method('org.bluez.MediaPlayer1')
    def Pause(self):
        self.fake.calls.append(('Pause', self._object_path))

    @dbus.service.method('org.bluez.MediaPlayer1')
    def Next(self):
        self.fake.calls.append(('Next', self._object_path))

    @dbus.service.method('org.bluez.MediaPlayer1')
    def Previous(self):
        self.fake.calls.append(('Previous', self._object_path))

    @dbus.service.method('org.bluez.MediaPlayer1')
    def Stop(self):
        self.fake.calls.append(('Stop', self._object_path))

    @dbus.service.method('org.bluez.Device1')
    def Disconnect(self):
        self.fake.calls.append(('Disconnect', self._object_path))

    @dbus.service.method('org.bluez.AgentManager1', in_signature='os')
    def RegisterAgent(self, path, cap):
        self.fake.calls.append(('RegisterAgent', str(path), str(cap)))

    @dbus.service.method('org.bluez.AgentManager1', in_signature='o')
    def RequestDefaultAgent(self, path):
        self.fake.calls.append(('RequestDefaultAgent', str(path)))

    @dbus.service.method(PROPS, in_signature='ssv')
    def Set(self, iface, name, value):
        self.fake.calls.append(('Set', self._object_path, str(name), value))

    @dbus.service.signal(PROPS, signature='sa{sv}as')
    def PropertiesChanged(self, iface, changed, invalidated):
        pass


class BtdTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bus = dbus.SessionBus()
        cls.name = dbus.service.BusName('org.bluez', cls.bus)
        cls.fake = Fake(cls.bus)
        cls.objs = {p: Obj(cls.fake, p) for p in (ADAPTER, DEVICE, PLAYER, '/org/bluez')}
        cls.loop = GLib.MainLoop()
        threading.Thread(target=cls.loop.run, daemon=True).start()
        cls.tmp = tempfile.mkdtemp()
        cls.sock_path = os.path.join(cls.tmp, 'bt.sock')
        cls.saved = os.path.join(cls.tmp, 'saved')
        cls.btd = subprocess.Popen([BTD, '--session', '--agent', '--socket', cls.sock_path,
                                    '--on-paired', 'touch ' + cls.saved],
                                   stderr=subprocess.PIPE)
        for _ in range(100):
            if os.path.exists(cls.sock_path):
                break
            time.sleep(0.05)

    @classmethod
    def tearDownClass(cls):
        cls.btd.terminate()
        cls.btd.wait(5)
        cls.loop.quit()

    def connect(self):
        s = socket.socket(socket.AF_UNIX)
        s.connect(self.sock_path)
        s.settimeout(3)
        return s, s.makefile('r')

    def expect(self, f, verb, timeout=3):
        end = time.time() + timeout
        while time.time() < end:
            line = f.readline()
            if line.startswith(verb + ' '):
                return line
        self.fail('no %r line' % verb)

    def wait_call(self, name, timeout=3):
        end = time.time() + timeout
        while time.time() < end:
            for c in self.fake.calls:
                if c[0] == name:
                    self.fake.calls.remove(c)
                    return c
            time.sleep(0.02)
        self.fail('BlueZ never got %s (calls: %r)' % (name, self.fake.calls))

    def changed(self, path, iface, props):
        GLib.idle_add(lambda: self.objs[path].PropertiesChanged(iface, props, []) and False)

    def test_1_snapshot_and_agent_registration(self):
        s, f = self.connect()
        bt = self.expect(f, 'bt')
        self.assertIn('connected=1', bt)
        self.assertIn('name="Dave\'s Pixel"', bt)
        self.assertIn('player=1', bt)
        self.assertIn('title=Thunderstruck', self.expect(f, 'track'))
        self.assertIn('status=paused', self.expect(f, 'play'))
        self.assertEqual(self.wait_call('RegisterAgent')[2], 'DisplayYesNo')
        self.wait_call('RequestDefaultAgent')
        s.close()

    def test_2_controls_reach_the_phone_player(self):
        s, f = self.connect()
        for cmd, method in (('play', 'Play'), ('pause', 'Pause'), ('next', 'Next'),
                            ('previous', 'Previous'), ('stop', 'Stop')):
            s.sendall((cmd + '\n').encode())
            self.assertEqual(self.wait_call(method)[1], PLAYER)
        s.close()

    def test_3_track_and_status_changes_are_pushed(self):
        s, f = self.connect()
        self.expect(f, 'play')
        self.changed(PLAYER, 'org.bluez.MediaPlayer1', {
            'Track': dbus.Dictionary({'Title': 'Hells Bells', 'Artist': 'AC/DC',
                                      'Album': 'Back in Black',
                                      'Duration': dbus.UInt32(312000)}, signature='sv')})
        self.assertIn('title="Hells Bells"', self.expect(f, 'track'))
        self.changed(PLAYER, 'org.bluez.MediaPlayer1',
                     {'Status': 'playing', 'Position': dbus.UInt32(5000)})
        play = self.expect(f, 'play')
        self.assertIn('status=playing', play)
        self.assertIn('position=5000', play)
        s.close()

    def test_4_pairing_confirm_and_reject(self):
        s, f = self.connect()
        s.sendall(b'pairable on\n')
        names = set()
        for _ in range(6):
            names.add(self.wait_call('Set')[2])
            if {'Pairable', 'Discoverable'} <= names:
                break
        self.assertTrue({'Pairable', 'Discoverable'} <= names)
        self.fake.calls.clear()
        btd_name = None
        for n in self.bus.list_names():
            if n.startswith(':') and n != self.bus.get_unique_name():
                btd_name = n
        agent = dbus.Interface(self.bus.get_object(btd_name, '/org/hogtied/agent'),
                               'org.bluez.Agent1')
        result = {}

        def ask(passkey):
            agent.RequestConfirmation(dbus.ObjectPath(DEVICE), dbus.UInt32(passkey),
                                      reply_handler=lambda: result.update(r='ok'),
                                      error_handler=lambda e: result.update(
                                          r=e.get_dbus_name()))
            return False

        GLib.idle_add(ask, 123456)
        pair = self.expect(f, 'pair')
        self.assertIn('passkey=123456', pair)
        self.assertIn("device=\"Dave's Pixel\"", pair)
        s.sendall(b'confirm yes\n')
        set_call = self.wait_call('Set')
        self.assertEqual((set_call[1], set_call[2]), (DEVICE, 'Trusted'))
        for _ in range(100):
            if 'r' in result:
                break
            time.sleep(0.02)
        self.assertEqual(result.get('r'), 'ok')

        result.clear()
        GLib.idle_add(ask, 654321)
        self.expect(f, 'pair')
        s.sendall(b'confirm no\n')
        self.assertIn('result=rejected', self.expect(f, 'pair-end'))
        for _ in range(100):
            if 'r' in result:
                break
            time.sleep(0.02)
        self.assertEqual(result.get('r'), 'org.bluez.Error.Rejected')
        s.close()

    def test_5_disconnect_clears_the_player(self):
        s, f = self.connect()
        self.expect(f, 'play')
        self.changed(DEVICE, 'org.bluez.Device1', {'Connected': False})
        bt = self.expect(f, 'bt')
        self.assertIn('connected=0', bt)
        self.assertIn('player=0', bt)
        s.sendall(b'next\n')                       # no player: nothing reaches BlueZ
        time.sleep(0.3)
        self.assertNotIn('Next', [c[0] for c in self.fake.calls])
        s.close()

    def test_5b_pairing_saves_keys(self):
        s, f = self.connect()
        self.changed(DEVICE, 'org.bluez.Device1', {'Paired': False})
        time.sleep(0.2)
        self.changed(DEVICE, 'org.bluez.Device1', {'Paired': True})
        self.assertIn('result=ok', self.expect(f, 'pair-end'))
        for _ in range(60):                       # runs ~3 s later
            if os.path.exists(self.saved):
                break
            time.sleep(0.1)
        self.assertTrue(os.path.exists(self.saved), 'on-paired command never ran')
        s.close()

    def test_6_bluetoothd_restart_resyncs(self):
        self.fake.calls.clear()
        self.bus.release_name('org.bluez')       # bluetoothd stops...
        time.sleep(0.3)
        self.bus.request_name('org.bluez')       # ...and starts again
        self.wait_call('RegisterAgent')           # agent registered again
        s, f = self.connect()
        self.assertIn('powered=1', self.expect(f, 'bt'))
        s.close()


if __name__ == '__main__':
    if not BTD:
        sys.exit(__doc__)
    unittest.main(argv=[sys.argv[0], '-v'])
