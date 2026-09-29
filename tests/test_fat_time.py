"""Host-тесты перевода штампов FAT (src/fat_time.cpp).

FAT хранит местное время без пояса; FTP (MDTM/MFMT/MLSD) и SMB (FILETIME)
требуют UTC. Модуль собирается MSVC вместе с tests/fat_time_host.cpp, а
ответы сверяются с эталоном на datetime Python — на граничных датах FAT,
високосных годах, невозможных полях и случайных значениях. Без Visual Studio
Build Tools тесты пропускаются.
"""
import datetime
import pathlib
import random
import subprocess
import unittest

from test_weather_parse import ROOT, build_exe

BUILD = ROOT / '.test-build' / 'fat_time'
UTC = datetime.timezone.utc
EPOCH = datetime.datetime(1970, 1, 1, tzinfo=UTC)
FILETIME_EPOCH = 116444736000000000


def fat_date(year: int, month: int, day: int) -> int:
    return (year - 1980) << 9 | month << 5 | day


def fat_time(hour: int, minute: int, second: int) -> int:
    return hour << 11 | minute << 5 | second // 2


def unix(moment: datetime.datetime) -> int:
    return int((moment - EPOCH).total_seconds())


class Host:
    """Одна запущенная программа — все команды через её stdin."""

    def __init__(self, exe: pathlib.Path) -> None:
        self.process = subprocess.Popen([str(exe)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True)

    def ask(self, line: str) -> str:
        assert self.process.stdin and self.process.stdout
        self.process.stdin.write(line + '\n')
        self.process.stdin.flush()
        return self.process.stdout.readline().strip()

    def close(self) -> None:
        if self.process.stdin:
            self.process.stdin.close()
        self.process.wait(timeout=10)


class FatTimeTest(unittest.TestCase):
    host: Host

    @classmethod
    def setUpClass(cls) -> None:
        exe = build_exe(BUILD, 'fat_time_host.exe', [ROOT / 'include'],
                        [(ROOT / 'src' / 'fat_time.cpp', 'fat_time.obj'),
                         (ROOT / 'tests' / 'fat_time_host.cpp', 'host.obj')])
        if exe is None:
            raise unittest.SkipTest('нет MSVC (vcvars64.bat): host-тест пропущен')
        cls.host = Host(exe)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.host.close()

    def unix2fat(self, moment: int, tz: int) -> tuple[int, int] | None:
        answer = self.host.ask(f'unix2fat {moment} {tz}')
        if answer == 'range':
            return None
        date, time = answer.split()
        return int(date), int(time)

    def fat2unix(self, date: int, time: int, tz: int) -> int | None:
        answer = self.host.ask(f'fat2unix {date} {time} {tz}')
        return None if answer == 'invalid' else int(answer)

    def test_local_stamp_becomes_utc_with_zifi_timezone(self) -> None:
        # Киев летом в zifi.ini: time:+3. 15.03.2024 14:30:20 на карте — это
        # 11:30:20 UTC.
        stamp = (fat_date(2024, 3, 15), fat_time(14, 30, 20))
        expected = unix(datetime.datetime(2024, 3, 15, 11, 30, 20, tzinfo=UTC))
        self.assertEqual(self.fat2unix(*stamp, 3 * 3600), expected)
        self.assertEqual(self.unix2fat(expected, 3 * 3600), stamp)

    def test_round_trip_matches_datetime_reference(self) -> None:
        rng = random.Random(20260926)
        first = unix(datetime.datetime(1980, 1, 2, tzinfo=UTC))
        last = unix(datetime.datetime(2107, 12, 30, tzinfo=UTC))
        for _ in range(3000):
            moment = rng.randrange(first, last)
            tz = rng.randrange(-12, 15) * 3600
            local = datetime.datetime.fromtimestamp(moment + tz, UTC)
            expected = (fat_date(local.year, local.month, local.day),
                        fat_time(local.hour, local.minute, local.second))
            with self.subTest(moment=moment, tz=tz):
                self.assertEqual(self.unix2fat(moment, tz), expected)
                # Обратно — с точностью до двух секунд FAT.
                self.assertEqual(self.fat2unix(*expected, tz), moment - local.second % 2)

    def test_fat_range_limits(self) -> None:
        self.assertEqual(self.unix2fat(unix(datetime.datetime(1980, 1, 1, tzinfo=UTC)), 0),
                         (fat_date(1980, 1, 1), 0))
        self.assertIsNone(self.unix2fat(unix(datetime.datetime(1979, 12, 31, 23, 59, 58,
                                                               tzinfo=UTC)), 0),
                          'дату раньше 1980 FAT не кодирует')
        self.assertEqual(self.unix2fat(unix(datetime.datetime(2107, 12, 31, 23, 59, 58,
                                                              tzinfo=UTC)), 0),
                         (fat_date(2107, 12, 31), fat_time(23, 59, 58)))
        self.assertIsNone(self.unix2fat(unix(datetime.datetime(2108, 1, 1, tzinfo=UTC)), 0))
        # Пояс сдвигает границу: 1980-01-01 00:30 UTC при time:-1 — ещё 1979.
        self.assertIsNone(self.unix2fat(unix(datetime.datetime(1980, 1, 1, 0, 30,
                                                               tzinfo=UTC)), -3600))

    def test_impossible_fields_are_not_a_date(self) -> None:
        cases = [
            (0, 0),                                   # неизвестно
            (fat_date(2023, 2, 29), 0),               # не високосный
            (fat_date(2024, 13, 1), 0),               # месяц 13
            (fat_date(2024, 0, 1), 0),                # месяц 0
            (fat_date(2024, 4, 31), 0),               # 31 апреля
            (fat_date(2024, 1, 0), 0),                # день 0
            (fat_date(2024, 1, 1), 24 << 11),         # 24 часа
            (fat_date(2024, 1, 1), 60 << 5),          # 60 минут
            (fat_date(2024, 1, 1), 30),               # 60 секунд
        ]
        for date, time in cases:
            with self.subTest(date=date, time=time):
                self.assertIsNone(self.fat2unix(date, time, 0))
        self.assertIsNotNone(self.fat2unix(fat_date(2024, 2, 29), 0, 0), 'високосный')
        self.assertIsNotNone(self.fat2unix(fat_date(2000, 2, 29), 0, 0), '2000 високосный')

    def test_ftp_timeval(self) -> None:
        expected = unix(datetime.datetime(2024, 3, 15, 11, 30, 20, tzinfo=UTC))
        self.assertEqual(self.host.ask('parse 20240315113020'), f'{expected} 14')
        self.assertEqual(self.host.ask('parse 20240315113020.123 /x'), f'{expected} 18')
        for bad in ('2024031511302', '20241315113020', '20240230000000',
                    '20240315113060', 'x0240315113020', '20240315113020.'):
            with self.subTest(bad=bad):
                self.assertEqual(self.host.ask(f'parse {bad}'), 'bad')
        self.assertEqual(self.host.ask(f'format {expected}'), '20240315113020')
        self.assertEqual(self.host.ask('format 0'), '19700101000000')

    def test_filetime(self) -> None:
        moment = unix(datetime.datetime(2024, 3, 15, 11, 30, 20, tzinfo=UTC))
        filetime = FILETIME_EPOCH + moment * 10_000_000
        self.assertEqual(self.host.ask(f'unix2ft {moment}'), str(filetime))
        self.assertEqual(self.host.ask(f'ft2unix {filetime + 9_999_999}'), str(moment))
        for special in (0, 2**64 - 1, 2**64 - 2, FILETIME_EPOCH - 1):
            with self.subTest(special=special):
                self.assertEqual(self.host.ask(f'ft2unix {special}'), 'special')


if __name__ == '__main__':
    unittest.main()
