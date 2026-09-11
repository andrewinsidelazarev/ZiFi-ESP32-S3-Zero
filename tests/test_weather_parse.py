"""Host-тест разбора ответов погодных сервисов (src/weather_parse.cpp).

Модуль собирается MSVC на ПК вместе с tests/weather_parse_host.cpp и
прогоняется на сохранённых ответах серверов из tests/fixtures/weather.
Без Visual Studio Build Tools тест пропускается.
"""
import datetime
import json
import math
import pathlib
import struct
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'tests' / 'fixtures' / 'weather'
BUILD = ROOT / '.test-build' / 'weather_parse'
VSWHERE = pathlib.Path(r'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe')

# Смещения записи — как в include/zifi/weather_parse.hpp.
REC_SIZE = 90
REC_PLACE = 2
REC_TEMP = 26
REC_DAYS = 42


def find_vcvars() -> pathlib.Path | None:
    if not VSWHERE.exists():
        return None
    result = subprocess.run([str(VSWHERE), '-products', '*', '-latest', '-find',
                             r'VC\Auxiliary\Build\vcvars64.bat'],
                            capture_output=True, text=True)
    path = result.stdout.strip().splitlines()
    return pathlib.Path(path[0]) if path else None


def short(path: pathlib.Path) -> str:
    """Короткий 8.3-путь: cmd.exe читает .bat в OEM-кодировке, и кириллица
    профиля в пути до исходников ломает команду."""
    import ctypes
    buffer = ctypes.create_unicode_buffer(32768)
    if not ctypes.windll.kernel32.GetShortPathNameW(str(path), buffer, len(buffer)):
        return str(path)
    return buffer.value


def build_host() -> pathlib.Path | None:
    vcvars = find_vcvars()
    if vcvars is None:
        return None
    BUILD.mkdir(parents=True, exist_ok=True)
    exe = BUILD / 'weather_parse_host.exe'
    script = BUILD / 'build_host.bat'
    # У обоих исходников одинаковое короткое имя WEATHE~1.CPP, поэтому каждый
    # компилируется отдельно в объект со своим именем.
    flags = f'/nologo /c /EHsc /std:c++17 /O2 /utf-8 /I "{short(ROOT / "include")}"'
    script.write_text(
        '@echo off\r\n'
        f'call "{short(vcvars)}" >nul 2>nul || exit /b 1\r\n'
        f'cl {flags} "{short(ROOT / "src" / "weather_parse.cpp")}" /Fo:parse.obj || exit /b 1\r\n'
        f'cl {flags} "{short(ROOT / "tests" / "weather_parse_host.cpp")}" /Fo:host.obj || exit /b 1\r\n'
        'cl /nologo parse.obj host.obj /Fe:weather_parse_host.exe\r\n',
        encoding='ascii')
    result = subprocess.run(['cmd.exe', '/c', short(script)], capture_output=True,
                            cwd=short(BUILD))
    if result.returncode != 0:
        output = result.stdout.decode('cp866', 'replace') + result.stderr.decode('cp866', 'replace')
        raise AssertionError(f'сборка host-теста не удалась:\n{output}')
    return exe


def rnd(value: float) -> int:
    """Округление как lround в прошивке: половина — от нуля (22.5 -> 23),
    а не банковское округление round() в Python."""
    return int(math.copysign(math.floor(abs(value) + 0.5), value))


def local_hm(unix: int, offset: int) -> tuple[int, int]:
    t = datetime.datetime.fromtimestamp(unix + offset, datetime.timezone.utc)
    return t.hour, t.minute


def local_date(unix: int, offset: int) -> tuple[int, int, int]:
    t = datetime.datetime.fromtimestamp(unix + offset, datetime.timezone.utc)
    return t.day, t.month, t.weekday()


class WeatherParseHostTest(unittest.TestCase):
    exe: pathlib.Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.exe = build_host()
        if cls.exe is None:
            raise unittest.SkipTest('MSVC не найден: host-тест разбора пропущен')

    def run_host(self, *args: str) -> str:
        result = subprocess.run([str(self.exe), *args], capture_output=True)
        output = result.stdout.decode('utf-8', 'replace').strip()
        self.assertEqual(result.returncode, 0, output)
        return output

    def test_zippopotam_takes_last_place(self) -> None:
        output = self.run_host('zip', str(FIXTURES / 'zippopotam_it_00144.json'))
        parts = output.split(' ', 3)
        self.assertEqual(parts[0], 'GEO')
        places = json.loads((FIXTURES / 'zippopotam_it_00144.json').read_text('utf-8'))['places']
        last = places[-1]
        self.assertAlmostEqual(float(parts[1]), float(last['latitude']), places=4)
        self.assertAlmostEqual(float(parts[2]), float(last['longitude']), places=4)
        self.assertEqual(parts[3], last['place name'])
        self.assertEqual(parts[3], 'Roma')

    def test_zippopotam_rejects_empty_reply(self) -> None:
        empty = BUILD / 'empty.json'
        empty.write_text('{}', encoding='utf-8')
        result = subprocess.run([str(self.exe), 'zip', str(empty)], capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn(b'ERR zip: no places', result.stdout)

    def test_open_meteo_record(self) -> None:
        fixture = FIXTURES / 'open_meteo_roma.json'
        output = self.run_host('meteo', str(fixture), 'Roma')
        self.assertTrue(output.startswith('REC '), output)
        record = bytes.fromhex(output[4:])
        self.assertEqual(len(record), REC_SIZE)
        src = json.loads(fixture.read_text('utf-8'))
        offset = src['utc_offset_seconds']
        cur = src['current']
        daily = src['daily']
        self.assertEqual(record[0], 1)
        self.assertEqual(record[1], 1)
        self.assertEqual(record[REC_PLACE:REC_PLACE + 24].split(b'\0')[0], b'Roma')
        temp, code, is_day, wind10, precip10, press = struct.unpack_from('<bBBHHH', record, REC_TEMP)
        self.assertEqual(temp, rnd(cur['temperature_2m']))
        self.assertEqual(code, cur['weather_code'])
        self.assertEqual(is_day, cur['is_day'])
        self.assertEqual(wind10, rnd(cur['wind_speed_10m'] * 10))
        self.assertEqual(precip10, rnd(cur['precipitation'] * 10))
        self.assertEqual(press, rnd(cur['surface_pressure'] * 0.750062))
        self.assertEqual(tuple(record[35:37]), local_hm(daily['sunrise'][0], offset))
        self.assertEqual(tuple(record[37:39]), local_hm(daily['sunset'][0], offset))
        self.assertEqual(tuple(record[39:41]), local_hm(cur['time'], offset))
        self.assertEqual(record[41], 6)
        for i in range(6):
            day = record[REC_DAYS + i * 8:REC_DAYS + (i + 1) * 8]
            d, m, wday = local_date(daily['time'][i] + 43200, offset)
            self.assertEqual((day[0], day[1], day[2]), (d, m, wday), f'день {i}')
            self.assertEqual(day[3], daily['weather_code'][i])
            tmin, tmax, dprecip = struct.unpack_from('<bbH', day, 4)
            self.assertEqual(tmin, rnd(daily['temperature_2m_min'][i]))
            self.assertEqual(tmax, rnd(daily['temperature_2m_max'][i]))
            self.assertEqual(dprecip, rnd(daily['precipitation_sum'][i] * 10))

    def test_place_from_utf8_file(self) -> None:
        # Название места с не-ASCII буквами передаётся файлом: так делает
        # модель ESP стенда Unreal (аргументы Windows приходят в ANSI).
        place = BUILD / 'place_utf8.txt'
        place.write_bytes('Città di Castello'.encode('utf-8'))
        output = self.run_host('meteo', str(FIXTURES / 'open_meteo_roma.json'), '@' + str(place))
        record = bytes.fromhex(output[4:])
        self.assertEqual(record[REC_PLACE:REC_PLACE + 24].split(b'\0')[0], b'Citta di Castello')

    def test_utf8_to_cp866(self) -> None:
        sample = BUILD / 'sample_utf8.txt'
        sample.write_bytes('Zürich Gréoux Москва Ёж 15°'.encode('utf-8'))
        output = self.run_host('cp866', str(sample))
        self.assertTrue(output.startswith('CP866 '))
        self.assertEqual(bytes.fromhex(output[6:]),
                         'Zurich Greoux Москва Ёж 15°'.encode('cp866'))


if __name__ == '__main__':
    unittest.main()
