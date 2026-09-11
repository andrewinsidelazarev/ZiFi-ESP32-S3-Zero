"""Host-тесты погоды в прошивке: разбор ответов (src/weather_parse.cpp) и служба
погоды целиком (src/weather_service.cpp).

Модули собираются MSVC на ПК: разбор — вместе с tests/weather_parse_host.cpp и
прогоняется на сохранённых ответах серверов из tests/fixtures/weather; служба —
с подменами Arduino, Wi-Fi и сети из tests/stubs_weather и сценариями
tests/weather_service_host.cpp. Без Visual Studio Build Tools тесты
пропускаются.
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
SERVICE_BUILD = ROOT / '.test-build' / 'weather_service'
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


def build_exe(build: pathlib.Path, exe_name: str, includes: list[pathlib.Path],
              sources: list[tuple[pathlib.Path, str]]) -> pathlib.Path | None:
    """Собрать программу MSVC. sources — (исходник, имя объекта): у исходников
    бывают одинаковые короткие имена (WEATHE~1.CPP), поэтому каждый
    компилируется отдельно в объект со своим именем."""
    vcvars = find_vcvars()
    if vcvars is None:
        return None
    build.mkdir(parents=True, exist_ok=True)
    exe = build / exe_name
    script = build / 'build_host.bat'
    flags = '/nologo /c /EHsc /std:c++17 /O2 /utf-8 ' + ' '.join(
        f'/I "{short(path)}"' for path in includes)
    lines = ['@echo off', f'call "{short(vcvars)}" >nul 2>nul || exit /b 1']
    lines += [f'cl {flags} "{short(source)}" /Fo:{obj} || exit /b 1' for source, obj in sources]
    lines.append('cl /nologo ' + ' '.join(obj for _, obj in sources) + f' /Fe:{exe_name}')
    script.write_text('\r\n'.join(lines) + '\r\n', encoding='ascii')
    result = subprocess.run(['cmd.exe', '/c', short(script)], capture_output=True,
                            cwd=short(build))
    if result.returncode != 0:
        output = result.stdout.decode('cp866', 'replace') + result.stderr.decode('cp866', 'replace')
        raise AssertionError(f'сборка {exe_name} не удалась:\n{output}')
    return exe


def build_host() -> pathlib.Path | None:
    return build_exe(BUILD, 'weather_parse_host.exe', [ROOT / 'include'],
                     [(ROOT / 'src' / 'weather_parse.cpp', 'parse.obj'),
                      (ROOT / 'tests' / 'weather_parse_host.cpp', 'host.obj')])


def build_service_host() -> pathlib.Path | None:
    # tests/stubs_weather стоит раньше include: служба получает поддельный
    # zifi/net_client.hpp и заглушки Arduino.h, WiFi.h, esp_heap_caps.h.
    return build_exe(SERVICE_BUILD, 'weather_service_host.exe',
                     [ROOT / 'tests' / 'stubs_weather', ROOT / 'include'],
                     [(ROOT / 'src' / 'weather_service.cpp', 'service.obj'),
                      (ROOT / 'src' / 'weather_parse.cpp', 'parse.obj'),
                      (ROOT / 'src' / 'config.cpp', 'config.obj'),
                      (ROOT / 'tests' / 'weather_service_host.cpp', 'scenarios.obj')])


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

    def rejected(self, mode: str, body: bytes) -> bytes:
        """Ответ, который разбор отвергает: строка вывода host-обёртки."""
        sample = BUILD / 'rejected.json'
        sample.write_bytes(body)
        result = subprocess.run([str(self.exe), mode, str(sample)], capture_output=True)
        self.assertEqual(result.returncode, 1, result.stdout)
        return result.stdout.strip()

    def geo(self, mode: str, fixture: str) -> list[str]:
        """Разобрать ответ справочника места: [GEO, широта, долгота, место]."""
        parts = self.run_host(mode, str(FIXTURES / fixture)).split(' ', 3)
        self.assertEqual(parts[0], 'GEO', parts)
        return parts

    def test_city_search_english(self) -> None:
        # city: Kyiv — поиск с language=en, название по-английски
        place = json.loads((FIXTURES / 'openmeteo_city_kyiv.json').read_text('utf-8'))['results'][0]
        parts = self.geo('city', 'openmeteo_city_kyiv.json')
        self.assertAlmostEqual(float(parts[1]), place['latitude'], places=4)
        self.assertAlmostEqual(float(parts[2]), place['longitude'], places=4)
        self.assertEqual(parts[3], 'Kyiv')

    def test_city_search_russian(self) -> None:
        # кириллица ищется с language=ru — и название приходит по-русски
        self.assertEqual(self.geo('city', 'openmeteo_city_rim_ru.json')[3], 'Рим')
        self.assertEqual(self.geo('city', 'openmeteo_city_rome_it.json')[3], 'Rome')

    def test_city_not_found(self) -> None:
        # ничего не нашёл — ответ без results; только это служба запоминает
        self.assertEqual(self.rejected('city', (FIXTURES / 'openmeteo_city_none.json').read_bytes()),
                         b'NOTFOUND city: not found')
        self.assertEqual(self.rejected('city', b'{"results":[]}'), b'NOTFOUND city: not found')

    def test_city_broken_reply(self) -> None:
        # испорченный ответ — ошибка, но не «места нет»: повтор может помочь
        self.assertEqual(self.rejected('city', b'[]'), b'ERR city: broken json')
        self.assertEqual(self.rejected('city', b'{"results":{}}'), b'ERR city: broken json')
        self.assertEqual(self.rejected('city', b'{"results":[{"name":"x",'), b'ERR city: broken json')
        self.assertEqual(self.rejected('city', b'{"results":[{"name":"x"}]}'),
                         b'ERR city: no coordinates')

    def test_zippopotam_takes_last_place(self) -> None:
        places = json.loads((FIXTURES / 'zippopotam_it_00144.json').read_text('utf-8'))['places']
        last = places[-1]
        parts = self.geo('zip', 'zippopotam_it_00144.json')
        self.assertAlmostEqual(float(parts[1]), float(last['latitude']), places=4)
        self.assertAlmostEqual(float(parts[2]), float(last['longitude']), places=4)
        self.assertEqual(parts[3], last['place name'])
        self.assertEqual(parts[3], 'Roma')

    def test_zippopotam_rejects_empty_reply(self) -> None:
        self.assertEqual(self.rejected('zip', b'{}'), b'ERR zip: no places')

    def test_ini_text_to_utf8(self) -> None:
        # zifi.ini правят и на ПК (UTF-8, CP1251 из Блокнота «ANSI»), и в
        # редакторе Wild Commander (CP866): геокодеру нужен UTF-8.
        cases = [
            ('Kyiv', 'Kyiv'.encode('ascii')),
            ('Рим', 'Рим'.encode('utf-8')),
            ('Рим', 'Рим'.encode('cp866')),
            ('Рим', 'Рим'.encode('cp1251')),
            ('Одесса', 'Одесса'.encode('cp866')),
            ('Одесса', 'Одесса'.encode('cp1251')),
            ('Нью-Йорк', 'Нью-Йорк'.encode('cp1251')),
            ('Київ', 'Київ'.encode('cp1251')),
            ('Київ', 'Київ'.encode('cp866')),
            ('Zürich', 'Zürich'.encode('utf-8')),
        ]
        for i, (expected, raw) in enumerate(cases):
            sample = BUILD / f'ini_{i}.txt'
            sample.write_bytes(raw)
            output = self.run_host('ini', str(sample))
            self.assertTrue(output.startswith('UTF8'), output)
            self.assertEqual(bytes.fromhex(output[5:]).decode('utf-8'), expected, raw)

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
        # Название места — в UTF-8, как его дал справочник; передаётся файлом:
        # так делает модель ESP стенда Unreal (аргументы Windows приходят в ANSI).
        for text, expected in (('Città di Castello', b'Citta di Castello'),
                               ('Одесса', 'Одесса'.encode('cp866'))):
            place = BUILD / 'place_utf8.txt'
            place.write_bytes(text.encode('utf-8'))
            output = self.run_host('meteo', str(FIXTURES / 'open_meteo_roma.json'), '@' + str(place))
            record = bytes.fromhex(output[4:])
            self.assertEqual(record[REC_PLACE:REC_PLACE + 24].split(b'\0')[0], expected)

    def test_utf8_to_cp866(self) -> None:
        sample = BUILD / 'sample_utf8.txt'
        sample.write_bytes('Zürich Gréoux Москва Ёж 15°'.encode('utf-8'))
        output = self.run_host('cp866', str(sample))
        self.assertTrue(output.startswith('CP866 '))
        self.assertEqual(bytes.fromhex(output[6:]),
                         'Zurich Greoux Москва Ёж 15°'.encode('cp866'))

    def test_utf8_to_cp866_ukrainian(self) -> None:
        # Є є Ї ї Ў ў в CP866 есть; І і пишутся латинскими I i (выглядят так
        # же), Ґ ґ — как Г г; типографские апостроф, кавычки и тире — ASCII.
        text = 'Київ Єнакієве Ґалаґан Ўсход Кам’янець «Ліс» — І'
        sample = BUILD / 'sample_ua.txt'
        sample.write_bytes(text.encode('utf-8'))
        output = self.run_host('cp866', str(sample))
        simple = text.translate(str.maketrans({'і': 'i', 'І': 'I', 'ґ': 'г', 'Ґ': 'Г',
                                               '’': "'", '«': '"', '»': '"', '—': '-'}))
        self.assertEqual(bytes.fromhex(output[6:]), simple.encode('cp866'))


class WeatherServiceHostTest(unittest.TestCase):
    """Служба погоды целиком: настоящий src/weather_service.cpp с поддельной
    сетью. Каждый тест — сценарий из tests/weather_service_host.cpp."""
    exe: pathlib.Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cls.exe = build_service_host()
        if cls.exe is None:
            raise unittest.SkipTest('MSVC не найден: host-тест службы погоды пропущен')

    def scenario(self, name: str) -> None:
        result = subprocess.run([str(self.exe), str(FIXTURES), name], capture_output=True)
        output = result.stdout.decode('utf-8', 'replace')
        lines = output.splitlines()
        self.assertFalse([line for line in lines if line.startswith('FAIL')], output)
        self.assertTrue([line for line in lines if line.startswith('OK')], output)
        self.assertEqual(result.returncode, 0, output)

    def test_city_english(self) -> None:
        self.scenario('city_en')

    def test_city_cyrillic_utf8(self) -> None:
        self.scenario('city_utf8')

    def test_city_cyrillic_cp866(self) -> None:
        self.scenario('city_cp866')

    def test_city_cyrillic_cp1251(self) -> None:
        self.scenario('city_cp1251')

    def test_city_with_country(self) -> None:
        self.scenario('city_country')

    def test_city_not_found_is_remembered(self) -> None:
        self.scenario('city_not_found')

    def test_zip_as_before(self) -> None:
        self.scenario('zip_legacy')

    def test_zip_not_found_is_remembered(self) -> None:
        self.scenario('zip_not_found')

    def test_busy_server_retried_not_remembered(self) -> None:
        self.scenario('city_503')

    def test_no_place_in_ini(self) -> None:
        self.scenario('no_place')

    def test_city_wins_over_zip(self) -> None:
        self.scenario('city_over_zip')

    def test_no_wifi(self) -> None:
        self.scenario('no_wifi')

    def test_forecast_error_keeps_place(self) -> None:
        self.scenario('meteo_503')

    def test_native_ukrainian_name(self) -> None:
        self.scenario('native_name')


if __name__ == '__main__':
    unittest.main()
