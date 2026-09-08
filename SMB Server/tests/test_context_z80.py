"""Execute the SMB WMF against a model of WC's public filesystem API.

Build with SMB Server/build.bat, then run this file with Python and z80 installed.
The model tracks mounted volumes separately from devices and panel directories.
It does not emulate a physical disk or prove real-Evo behavior.
"""
from pathlib import Path
import re
import unittest
import z80

PROJECT = Path(__file__).resolve().parents[1]
SYMBOLS = {name: int(value, 16) for name, value in re.findall(
    r'^([^\s:]+): EQU 0x([0-9A-Fa-f]+)$',
    (PROJECT / 'build/ZIFISMB.sym').read_text(), re.M)}
IMAGE = (PROJECT / 'build/ZIFISMB.WMF').read_bytes()
API, STOP, STACK = 0x6006, 0x0100, 0x5FFC
INI = b'ssid: Test\npassword: example\n'


class Commander:
    def __init__(self, active, volumes, devices):
        self.cpu = z80.Z80Machine()
        self.cpu.memory[0x8000:0x8000 + len(IMAGE) - 512] = IMAGE[512:]
        self.cpu.set_breakpoint(API)
        self.cpu.set_breakpoint(STOP)
        self.panel = (active, '/panel/subdir')
        self.volumes = volumes
        self.devices = devices
        self.streams = {}
        self.selected = None
        self.entry = None
        self.clone_count = 0
        self.probes = []
        self.read_volumes = []

    def flags(self, success):
        self.cpu.a = 0 if success else 1
        self.cpu.f = 0x40 if success else 0

    def string(self, pointer):
        data = bytearray()
        while self.cpu.memory[pointer]:
            data.append(self.cpu.memory[pointer])
            pointer += 1
        return data.decode('ascii')

    def ret(self):
        m, sp = self.cpu.memory, self.cpu.sp
        self.cpu.pc = m[sp] | m[sp + 1] << 8
        self.cpu.sp += 2

    def api(self):
        c = self.cpu
        if c.a == 57:
            stream = c.d
            if stream == 0xFE:
                # MD20 STREAM #FE copies the panel snapshot in FEP2 to F3/F4.
                self.streams = {0: self.panel, 1: self.panel}
                self.selected = 0
                self.clone_count += 1
                self.flags(True)
            elif stream == 0xFF:
                volume, _ = self.streams[self.selected]
                self.streams[self.selected] = (volume, '/')
                self.flags(True)
            elif c.bc == 0xFFFF:
                assert stream in self.streams
                self.selected = stream
                self.flags(True)
            else:
                self.probes.append(c.b)
                volume = self.devices.get(c.b)
                if volume is not None:
                    self.streams[stream] = (volume, '/')
                    self.selected = stream
                self.flags(volume is not None)
            c.bc = 0  # STREAM does not preserve the caller's BC.
        elif c.a == 59:
            volume, directory = self.streams[self.selected]
            attribute = c.memory[c.hl]
            name = self.string(c.hl + 1)
            path = directory.rstrip('/') + '/' + name
            data = self.volumes.get(volume, {}).get(path)
            found = data is not None
            self.entry = (volume, path, data)
            c.a, c.f = (1, 0) if found else (0, 0x40)  # FENTRY uses NZ for success.
            c.de = 0
            c.hl = len(data) if found and not attribute else 0
        elif c.a == 63:
            volume, path, _ = self.entry
            self.streams[self.selected] = (volume, path)
        elif c.a == 62:
            assert self.entry[2] is not None
        elif c.a == 48:
            volume, _, data = self.entry
            self.read_volumes.append(volume)
            block = data.ljust(c.b * 512, b'\0')
            c.memory[c.hl:c.hl + len(block)] = block
            self.flags(True)
        else:
            raise AssertionError('Unexpected WC API: ' + str(c.a))
        self.ret()

    def call(self, symbol):
        self.cpu.pc, self.cpu.sp = SYMBOLS[symbol], STACK
        self.cpu.memory[STACK:STACK + 2] = STOP.to_bytes(2, 'little')
        for _ in range(1000):
            self.cpu.ticks_to_stop = 1000000
            self.cpu.run()
            if self.cpu.pc == STOP:
                self.assert_returned()
                return
            if self.cpu.pc == API:
                self.api()
        raise AssertionError('Execution did not return from ' + symbol)

    def assert_returned(self):
        assert self.cpu.sp == STACK + 2


def files():
    return {'/zifi': b'', '/zifi/zifi.ini': INI}


class VolumeTests(unittest.TestCase):
    def assert_volume(self, wc, volume):
        self.assertEqual(wc.streams, {0: (volume, '/'), 1: (volume, '/')})
        self.assertEqual(wc.selected, 0)
        self.assertEqual(wc.panel, (volume, '/panel/subdir'))

    def test_active_device_and_mounted_partition(self):
        for device in range(7):
            with self.subTest(device=device):
                active = (device, 'second-partition')
                wc = Commander(active, {active: files()}, {0: (1, 'first-partition')})
                wc.cpu.memory[SYMBOLS['ConfigPanelDevice']] = device
                wc.call('Config_Load')
                self.assertFalse(wc.cpu.f & 1)
                self.assertEqual(wc.probes, [])
                self.assertEqual(wc.read_volumes, [active])
                self.assertEqual(wc.clone_count, 2)
                self.assert_volume(wc, active)

    def test_ini_on_sd1_does_not_publish_sd1(self):
        active, sd = (3, 'mounted-ide-partition'), (1, 'sd1-partition')
        wc = Commander(active, {active: {}, sd: files()}, {0: sd})
        wc.call('Config_Load')
        self.assertFalse(wc.cpu.f & 1)
        self.assertEqual(wc.read_volumes, [sd])
        self.assertEqual(bytes(wc.cpu.memory[SYMBOLS['IniBuffer']:SYMBOLS['IniBuffer'] + len(INI)]), INI)
        self.assert_volume(wc, active)
        wc.streams[0] = (active, '/server/deep/path')
        wc.call('Fs_ResetRoot')
        self.assert_volume(wc, active)
        self.assertEqual(wc.clone_count, 2)

    def test_ini_on_sd2_after_unavailable_sd1(self):
        active, sd = (5, 'mounted-smuc-partition'), (6, 'sd2-partition')
        wc = Commander(active, {active: {}, sd: files()}, {6: sd})
        wc.call('Config_Load')
        self.assertFalse(wc.cpu.f & 1)
        self.assertEqual(wc.probes, [0, 6, 6])
        self.assertEqual(wc.read_volumes, [sd])
        self.assert_volume(wc, active)

    def test_missing_ini_keeps_original_volume(self):
        active, sd = (0, 'mounted-ide-partition'), (1, 'sd1-partition')
        wc = Commander(active, {active: {}, sd: {'/zifi': b''}}, {0: sd})
        wc.call('Config_Load')
        self.assertTrue(wc.cpu.f & 1)
        self.assertEqual(wc.cpu.memory[SYMBOLS['ConfigError']], 3)
        self.assertEqual(wc.read_volumes, [])
        self.assert_volume(wc, active)

    def test_no_zifi_directory_keeps_original_volume(self):
        active = (4, 'mounted-smuc-partition')
        wc = Commander(active, {active: {}}, {})
        wc.call('Config_Load')
        self.assertTrue(wc.cpu.f & 1)
        self.assertEqual(wc.cpu.memory[SYMBOLS['ConfigError']], 2)
        self.assert_volume(wc, active)

    def test_exit_restores_panel_ix_and_requests_refresh_after_ui_close(self):
        cpu = z80.Z80Machine()
        cpu.memory[0x8000:0x8000 + len(IMAGE) - 512] = IMAGE[512:]
        for address in (API, STOP, SYMBOLS['Link_Stop']):
            cpu.set_breakpoint(address)
        cpu.pc, cpu.sp = SYMBOLS['PLUGIN.exit'], STACK
        original_ix = 0x7EA0
        cpu.memory[STACK:STACK + 4] = original_ix.to_bytes(2, 'little') + STOP.to_bytes(2, 'little')
        events = []
        for _ in range(100):
            cpu.ticks_to_stop = 1000000
            cpu.run()
            if cpu.pc == STOP:
                break
            if cpu.pc == SYMBOLS['Link_Stop']:
                events.append('stop')
            elif cpu.pc == API:
                events.append(cpu.a)
                cpu.a = 0xAA  # API flags/registers must not replace the exit code.
                cpu.ix = 0x1234
            else:
                self.fail('Unexpected breakpoint')
            cpu.pc = cpu.memory[cpu.sp] | cpu.memory[cpu.sp + 1] << 8
            cpu.sp += 2
        self.assertEqual(cpu.pc, STOP)
        self.assertEqual(events, ['stop', 2, 15])
        self.assertEqual(cpu.a, 3)
        self.assertEqual(cpu.ix, original_ix)
        self.assertEqual(cpu.sp, STACK + 4)


if __name__ == '__main__':
    unittest.main(verbosity=2)
