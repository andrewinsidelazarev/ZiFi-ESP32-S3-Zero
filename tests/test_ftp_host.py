"""Сквозная проверка дат FTP-сервера прошивки на ПК.

Настоящий FtpServer (src/ftp_server.cpp) вместе с VfsBridge и VfsClient
собирается MSVC в стенд tools/host_ftp. Вместо Z80 за «UART» стоит эмулятор
Wild Commander поверх обычной папки (tools/host_smb/z80_sim.cpp), вместо
Wi-Fi — сокеты Windows. Клиент — обычный ftplib.

Проверяется, что даты файлов доходят до LIST, MLSD, MLST и MDTM; что MFMT
записывает дату в файл; что пояс из zifi.ini сдвигает UTC в нужную сторону.
С эмулятором старых плагинов (ZIFI_SIM_NO_DATES) сервер обязан обходиться без
дат и отказывать в MFMT, не трогая файл: старый FTP-плагин понял бы режим
OPEN=3 как запись и удалил бы его. Без Visual Studio Build Tools тесты
пропускаются.
"""
import datetime
import ftplib
import os
import pathlib
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

from test_weather_parse import ROOT, find_vcvars, short

BUILD = ROOT / '.test-build' / 'host_ftp'
UTC = datetime.timezone.utc
SOURCES = [
    ('tools/host_ftp/main.cpp', 'main.obj'),
    ('tools/host_smb/z80_sim.cpp', 'z80_sim.obj'),
    ('tools/host_smb/crash_report.cpp', 'crash_report.obj'),
    ('src/ftp_server.cpp', 'ftp_server.obj'),
    ('src/vfs_bridge.cpp', 'vfs_bridge.obj'),
    ('src/vfs_client.cpp', 'vfs_client.obj'),
    ('src/fat_allocation_cache.cpp', 'fat_allocation_cache.obj'),
    ('src/spsc_ring.cpp', 'spsc_ring.obj'),
    ('src/protocol.cpp', 'protocol.obj'),
    ('src/uart_transport.cpp', 'uart_transport.obj'),
    ('src/fat_time.cpp', 'fat_time.obj'),
]
INCLUDES = ['tools/host_ftp/stubs', 'tests/stubs_host', 'include', 'tools/host_smb']


def build_host_ftp() -> pathlib.Path | None:
    vcvars = find_vcvars()
    if vcvars is None:
        return None
    BUILD.mkdir(parents=True, exist_ok=True)
    flags = ('/nologo /c /EHsc /std:c++17 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS '
             '/DZIFI_HOST_BUILD /D_WINDOWS /FIzifi_msvc_prelude.h ' +
             ' '.join(f'/I "{short(ROOT / path)}"' for path in INCLUDES))
    lines = ['@echo off', f'call "{short(vcvars)}" >nul 2>nul || exit /b 1']
    lines += [f'cl {flags} "{short(ROOT / source)}" /Fo:{obj} || exit /b 1'
              for source, obj in SOURCES]
    lines.append('cl /nologo ' + ' '.join(obj for _, obj in SOURCES) +
                 ' /Fe:host_ftp.exe /link ws2_32.lib')
    script = BUILD / 'build_host.bat'
    script.write_text('\r\n'.join(lines) + '\r\n', encoding='ascii')
    result = subprocess.run(['cmd.exe', '/c', short(script)], capture_output=True,
                            cwd=short(BUILD))
    if result.returncode != 0:
        output = (result.stdout.decode('cp866', 'replace') +
                  result.stderr.decode('cp866', 'replace'))
        raise AssertionError(f'сборка host_ftp.exe не удалась:\n{output}')
    return BUILD / 'host_ftp.exe'


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def stamp(year, month, day, hour, minute, second) -> float:
    return datetime.datetime(year, month, day, hour, minute, second,
                             tzinfo=UTC).timestamp()


class HostFtpDatesTest(unittest.TestCase):
    exe: pathlib.Path

    GAME = stamp(2024, 3, 15, 11, 30, 20)
    OLD = stamp(2001, 5, 6, 7, 8, 10)

    @classmethod
    def setUpClass(cls) -> None:
        exe = build_host_ftp()
        if exe is None:
            raise unittest.SkipTest('нет MSVC (vcvars64.bat): стенд FTP пропущен')
        cls.exe = exe

    def setUp(self) -> None:
        self.root = pathlib.Path(tempfile.mkdtemp(prefix='zifi_ftp_'))
        (self.root / 'GAME.TAP').write_bytes(b'x' * 1234)
        os.utime(self.root / 'GAME.TAP', (self.GAME, self.GAME))
        (self.root / 'OLD.TXT').write_bytes(b'old')
        os.utime(self.root / 'OLD.TXT', (self.OLD, self.OLD))
        # Свежий файл — вчерашний: LIST показывает для него часы, а не год.
        self.recent = round(time.time()) // 2 * 2 - 86400
        (self.root / 'RECENT.TXT').write_bytes(b'new')
        os.utime(self.root / 'RECENT.TXT', (self.recent, self.recent))
        (self.root / 'DOCS').mkdir()
        self.process = None
        self.ftp = None

    def tearDown(self) -> None:
        if self.ftp is not None:
            try:
                self.ftp.quit()
            except (OSError, ftplib.Error, EOFError):
                pass
        if self.process is not None:
            self.process.kill()
            self.process.wait(timeout=10)
        shutil.rmtree(self.root, ignore_errors=True)

    def start(self, timezone_hours: int = 0, dates: bool = True) -> ftplib.FTP:
        port = free_port()
        environment = dict(os.environ)
        environment.pop('ZIFI_SIM_NO_DATES', None)
        if not dates:
            environment['ZIFI_SIM_NO_DATES'] = '1'
        self.process = subprocess.Popen(
            [str(self.exe), str(self.root), str(port), str(timezone_hours)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=environment)
        deadline = time.monotonic() + 15
        while True:
            try:
                ftp = ftplib.FTP()
                ftp.connect('127.0.0.1', port, timeout=10)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.2)
        ftp.login('zx', 'zx')
        self.ftp = ftp
        return ftp

    def listing(self, ftp: ftplib.FTP) -> dict[str, str]:
        lines: list[str] = []
        ftp.retrlines('LIST', lines.append)
        # В именах тестовых файлов пробелов нет, а в дате «Mar 15  2024» их
        # два подряд — поэтому имя берётся последним полем.
        return {line.rsplit(' ', 1)[-1]: line for line in lines}

    def test_dates_reach_list_mlsd_mlst_and_mdtm(self) -> None:
        ftp = self.start()
        features = ftp.sendcmd('FEAT')
        for feature in ('MDTM', 'MFMT', 'MLST type*;size*;modify*;'):
            self.assertIn(feature, features)

        facts = {name: data for name, data in ftp.mlsd()}
        self.assertEqual(facts['GAME.TAP']['modify'], '20240315113020')
        self.assertEqual(facts['GAME.TAP']['size'], '1234')
        self.assertEqual(facts['GAME.TAP']['type'], 'file')
        self.assertEqual(facts['OLD.TXT']['modify'], '20010506070810')
        self.assertEqual(facts['DOCS']['type'], 'dir')
        self.assertIn('modify', facts['DOCS'])

        lines = self.listing(ftp)
        self.assertIn(' 1234 Mar 15  2024 GAME.TAP', lines['GAME.TAP'])
        self.assertIn(' May  6  2001 OLD.TXT', lines['OLD.TXT'])
        recent = datetime.datetime.fromtimestamp(self.recent, UTC)
        self.assertIn(recent.strftime(' %b ') + f'{recent.day:2d}' +
                      recent.strftime(' %H:%M RECENT.TXT'), lines['RECENT.TXT'])

        self.assertEqual(ftp.sendcmd('MDTM GAME.TAP'), '213 20240315113020')
        self.assertIn('modify=20240315113020; /GAME.TAP', ftp.sendcmd('MLST GAME.TAP'))

    def test_mfmt_writes_modification_time_into_file(self) -> None:
        ftp = self.start()
        self.assertEqual(ftp.sendcmd('MFMT 20200102030406 GAME.TAP'),
                         '213 Modify=20200102030406; GAME.TAP')
        self.assertEqual(os.stat(self.root / 'GAME.TAP').st_mtime,
                         stamp(2020, 1, 2, 3, 4, 6))
        self.assertEqual(ftp.sendcmd('MDTM GAME.TAP'), '213 20200102030406')
        # FAT хранит секунды с шагом 2: ответ называет то, что записано.
        self.assertEqual(ftp.sendcmd('MFMT 20200102030407 GAME.TAP'),
                         '213 Modify=20200102030406; GAME.TAP')
        self.assertEqual((self.root / 'GAME.TAP').read_bytes(), b'x' * 1234)
        with self.assertRaises(ftplib.error_perm):
            ftp.sendcmd('MFMT 19791231235958 GAME.TAP')      # FAT не кодирует
        with self.assertRaises(ftplib.error_perm):
            ftp.sendcmd('MFMT 2020010203040 GAME.TAP')
        with self.assertRaises(ftplib.error_perm):
            ftp.sendcmd('MFMT 20200102030406 NOSUCH.BIN')

    def test_timezone_from_zifi_ini_shifts_utc(self) -> None:
        # На карте местное время: при time:+3 штамп 11:30:20 — это 08:30:20 UTC.
        ftp = self.start(timezone_hours=3)
        self.assertEqual(ftp.sendcmd('MDTM GAME.TAP'), '213 20240315083020')
        facts = {name: data for name, data in ftp.mlsd()}
        self.assertEqual(facts['GAME.TAP']['modify'], '20240315083020')
        self.assertIn(' Mar 15  2024 GAME.TAP', self.listing(ftp)['GAME.TAP'])
        # И обратно: 03:04:06 UTC ложится на карту как 06:04:06 местного.
        ftp.sendcmd('MFMT 20200102030406 GAME.TAP')
        self.assertEqual(os.stat(self.root / 'GAME.TAP').st_mtime,
                         stamp(2020, 1, 2, 6, 4, 6))
        self.assertEqual(ftp.sendcmd('MDTM GAME.TAP'), '213 20200102030406')

    def test_old_plugin_without_dates(self) -> None:
        ftp = self.start(dates=False)
        facts = {name: data for name, data in ftp.mlsd()}
        self.assertEqual(facts['GAME.TAP']['size'], '1234')
        self.assertNotIn('modify', facts['GAME.TAP'])
        self.assertIn(' 1234 Jan 01 00:00 GAME.TAP', self.listing(ftp)['GAME.TAP'])
        with self.assertRaises(ftplib.error_perm):
            ftp.sendcmd('MDTM GAME.TAP')
        with self.assertRaises(ftplib.error_perm):
            ftp.sendcmd('MFMT 20200102030406 GAME.TAP')
        self.assertEqual((self.root / 'GAME.TAP').read_bytes(), b'x' * 1234)
        self.assertEqual(os.stat(self.root / 'GAME.TAP').st_mtime, self.GAME)


if __name__ == '__main__':
    unittest.main()
