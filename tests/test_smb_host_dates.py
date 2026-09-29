"""Сквозная проверка дат SMB-сервера прошивки на ПК.

Стенд tools/host_smb собирает НАСТОЯЩИЙ SmbServer вместе с libsmb2, VfsBridge и
VfsClient; вместо Z80 — эмулятор Wild Commander поверх папки. Клиент — режим
dates программы tools/host_smb/smb_reproduce_test.c (libsmb2):
- время изменения файла доходит до клиента и в листинге (QUERY_DIRECTORY), и
  в свойствах (QUERY_INFO), сдвинутое поясом ESP из местного в UTC;
- SET_INFO кладёт на карту местное время (UTC плюс пояс), а сервер после
  этого показывает новое значение и в свойствах, и в листинге.
Сборка стенда: tools/host_smb/build.ps1 (нужны Visual Studio Build Tools).
"""
import os
import pathlib
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / '.test-build' / 'host_smb'
SERVER = BUILD / 'host_smb.exe'
CLIENT = BUILD / 'smb_reproduce_test.exe'
LISTED = 1710502220          # 2024-03-15 11:30:20 — штамп на «карте»
SET_INFO = 1700000000        # 2023-11-14 22:13:20 UTC — ставит клиент


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


class HostSmbDatesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not SERVER.exists() or not CLIENT.exists():
            script = ROOT / 'tools' / 'host_smb' / 'build.ps1'
            result = subprocess.run(
                ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                 '-File', str(script)], capture_output=True, cwd=ROOT)
            if result.returncode != 0 or not SERVER.exists():
                raise unittest.SkipTest('SMB-стенд не собран (нужны Build Tools)')

    def run_dates(self, timezone_hours: int) -> None:
        share = pathlib.Path(tempfile.mkdtemp(prefix='zifi_smb_'))
        server = None
        try:
            target = share / 'dates.bin'
            target.write_bytes(b'date test')
            os.utime(target, (LISTED, LISTED))
            port = free_port()
            environment = dict(os.environ)
            environment['ZIFI_HOST_TZ_HOURS'] = str(timezone_hours)
            environment['ZIFI_TEST_TZ_HOURS'] = str(timezone_hours)
            environment.pop('ZIFI_SIM_NO_DATES', None)
            server = subprocess.Popen([str(SERVER), str(share), str(port)],
                                      stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL, env=environment)
            deadline = time.monotonic() + 15
            while True:
                try:
                    socket.create_connection(('127.0.0.1', port), timeout=1).close()
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(0.2)
            result = subprocess.run(
                [str(CLIENT), f'127.0.0.1:{port}', 'SD', 'dates'],
                capture_output=True, text=True, timeout=120, env=environment)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn('SUCCESS: dates', result.stdout)
            # SET_INFO: на карту легло местное время — UTC плюс пояс.
            self.assertEqual(os.stat(target).st_mtime,
                             SET_INFO + timezone_hours * 3600)
        finally:
            if server is not None:
                server.kill()
                server.wait(timeout=10)
            shutil.rmtree(share, ignore_errors=True)

    def test_dates_utc(self) -> None:
        self.run_dates(0)

    def test_dates_with_timezone(self) -> None:
        self.run_dates(3)


if __name__ == '__main__':
    unittest.main()
