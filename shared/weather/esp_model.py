"""Модель ESP32-S3 для стенда Unreal (ZiFi=UART) — на обычном Python.

Unreal создаёт именованный канал, кладёт его имя в переменную окружения
MICROPY_UART_PIPE и запускает эту программу; байты канала — это UART ZiFi.
Модель отвечает на команды заставки погоды так же, как прошивка:

  PING (#04)        -> READY (#F0);
  WIFI_INI (#03)    -> ACK, затем #83 [1][IPv4] и запоминает ключи ini;
  WEATHER_GET (#24) -> ACK, затем #A4 с записью погоды либо #EE + #A4 [0][1].

Погода настоящая: те же запросы, что делает прошивка (место по city: —
геокодер geocoding-api.open-meteo.com, по zip: — api.zippopotam.us, прогноз —
api.open-meteo.com), а ответы и значения ini разбирает сам код прошивки
src/weather_parse.cpp, собранный host-тестом в weather_parse_host.exe. Путь к
нему модель берёт из esp_model.json рядом с собой (его пишет
unreal_bench.py). Остальные команды получают доклад «unsupported», как в
прошивке. Журнал — esp_model.log в каталоге запуска.
"""
import json
import os
import subprocess
import sys
import tempfile
import time
import traceback
import urllib.parse
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOG = HERE / 'esp_model.log'
CONFIG = json.loads((HERE / 'esp_model.json').read_text(encoding='utf-8'))
HOST_EXE = CONFIG['weather_parse_host']
USER_AGENT = 'ZiFi (ZX Evo)'           # как у прошивки (src/net_client.cpp)
CITY_URL = 'http://geocoding-api.open-meteo.com/v1/search?'
ZIP_URL = 'http://api.zippopotam.us/'
FORECAST_URL = 'http://api.open-meteo.com'
FORECAST_PATH = ('/v1/forecast?latitude=%.4f&longitude=%.4f'
                 '&current=temperature_2m,weather_code,is_day,wind_speed_10m,'
                 'precipitation,surface_pressure'
                 '&daily=weather_code,temperature_2m_max,temperature_2m_min,'
                 'precipitation_sum,sunrise,sunset'
                 '&timezone=auto&forecast_days=6&timeformat=unixtime')


def log(text):
    with LOG.open('a', encoding='utf-8') as stream:
        stream.write(time.strftime('%H:%M:%S ') + text + '\n')


def frame(command, payload=b''):
    """Пакет протокола: SYNC, CMD, длина LE16, данные, XOR от CMD до конца."""
    body = bytes([command, len(payload) & 0xFF, len(payload) >> 8]) + payload
    checksum = 0
    for byte in body:
        checksum ^= byte
    return b'\x5A' + body + bytes([checksum])


def parse_ini(data):
    """Ключи zifi.ini как у IniConfig::parse: «ключ: значение», регистр ключа
    не важен, ';' и '#' — комментарии, значение можно взять в кавычки.
    Значения — байты как есть (latin-1 без потерь): название города могло
    быть в UTF-8, CP866 или CP1251, его переводит в UTF-8 код прошивки."""
    result = {}
    if data.startswith(b'\xef\xbb\xbf'):
        data = data[3:]
    text = data.decode('latin-1')
    for line in text.replace('\r', '\n').split('\n'):
        line = line.strip()
        if not line or line[0] in ';#' or ':' not in line:
            continue
        key, value = line.split(':', 1)
        value = value.strip()
        if value.startswith('"'):
            value = value[1:].split('"', 1)[0]
        result[key.strip().lower()] = value
    return result


class HttpError(RuntimeError):
    """Сервер ответил не 2xx; code — код ответа."""

    def __init__(self, code):
        super().__init__('http %d' % code)
        self.code = code


def http_get(url):
    request = urllib.request.Request(url, headers={
        'User-Agent': USER_AGENT, 'Accept': '*/*', 'Accept-Encoding': 'identity'})
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            return response.read()
    except urllib.error.HTTPError as error:
        raise HttpError(error.code) from None


class HostError(RuntimeError):
    """Разбор прошивки отверг ответ; not_found — места нет (NOTFOUND)."""

    def __init__(self, text, not_found=False):
        super().__init__(text)
        self.not_found = not_found


def run_host(*args):
    result = subprocess.run([HOST_EXE, *args], capture_output=True)
    output = result.stdout.decode('utf-8', 'replace').strip()
    if result.returncode != 0 or output.startswith('ERR'):
        kind, _, text = output.partition(' ')
        raise HostError(text or output, not_found=kind == 'NOTFOUND')
    return output


class Model:
    def __init__(self):
        self.ini = {}
        self.coordinates = {}          # ключ места -> (широта, долгота, место UTF-8)
        self.unknown = set()           # места, которых справочник не знает
        self.work = Path(tempfile.gettempdir())

    def find_city(self, city, country):
        """Название -> место, как WeatherService::geocodeCity в прошивке."""
        raw = self.work / 'esp_model_city.txt'
        raw.write_bytes(city.encode('latin-1'))          # байты значения из ini
        name = bytes.fromhex(run_host('ini', str(raw))[5:]).decode('utf-8')
        cyrillic = any(0x400 <= ord(ch) <= 0x4FF for ch in name)   # кириллица — ищем по-русски
        query = ('name=' + urllib.parse.quote(name, safe='-') + '&count=1&language=' +
                 ('ru' if cyrillic else 'en') + '&format=json')
        if country:
            query += '&countryCode=' + urllib.parse.quote(country, safe='-')
        try:
            body = http_get(CITY_URL + query)
        except Exception as error:
            raise RuntimeError('city: %s' % error) from None
        source = self.work / 'esp_model_geo.json'
        source.write_bytes(body)
        return run_host('city', str(source)).split(' ', 3)

    def find_zip(self, country, zip_code):
        """Почтовый индекс -> место, как WeatherService::geocodeZip."""
        try:
            body = http_get(ZIP_URL + urllib.parse.quote(country, safe='-') + '/' +
                            urllib.parse.quote(zip_code, safe='-'))
        except HttpError as error:
            if error.code == 404:
                raise HostError('zip: not found', not_found=True) from None
            raise RuntimeError('zip: %s' % error) from None
        except Exception as error:
            raise RuntimeError('zip: %s' % error) from None
        source = self.work / 'esp_model_geo.json'
        source.write_bytes(body)
        return run_host('zip', str(source)).split(' ', 3)

    def weather(self):
        city = self.ini.get('city', '')
        country = self.ini.get('country', '')
        zip_code = self.ini.get('zip', '')
        if not city and not (zip_code and country):
            raise RuntimeError('no city in ini')
        key = ('city:%s/%s' % (country, city)) if city else ('zip:%s/%s' % (country, zip_code))
        if key in self.unknown:
            # как прошивка: этот справочник место уже не нашёл — без запроса
            raise RuntimeError('city: not found' if city else 'zip: not found')
        if key not in self.coordinates:
            try:
                geo = self.find_city(city, country) if city else self.find_zip(country, zip_code)
            except HostError as error:
                if error.not_found:
                    self.unknown.add(key)
                raise
            self.coordinates[key] = (float(geo[1]), float(geo[2]), geo[3])
            log('geo %s -> %s %s %s' % ((key.encode('latin-1').decode('utf-8', 'replace'),) +
                                        self.coordinates[key]))
        work = self.work
        latitude, longitude, place = self.coordinates[key]
        try:
            body = http_get(FORECAST_URL + FORECAST_PATH % (latitude, longitude))
        except Exception as error:
            raise RuntimeError('meteo: %s' % error) from None
        source = work / 'esp_model_meteo.json'
        source.write_bytes(body)
        place_file = work / 'esp_model_place.txt'
        place_file.write_bytes(place.encode('utf-8'))
        record = bytes.fromhex(run_host('meteo', str(source), '@' + str(place_file))[4:])
        log('weather record %d bytes, temp %d, code %d' % (
            len(record), int.from_bytes(record[26:27], 'little', signed=True), record[27]))
        return record

    def handle(self, command, payload):
        """Ответ на один пакет: список кадров для отправки."""
        if command == 0x04:
            return [frame(0xF0)]
        if command == 0x03:
            self.ini = parse_ini(payload)
            log('WIFI_INI keys: %s' % ', '.join(sorted(self.ini)))
            return [frame(0xFE), frame(0x83, bytes([1, 10, 0, 2, 15]))]
        if command == 0x24:
            try:
                return [frame(0xFE), frame(0xA4, self.weather())]
            except Exception as error:
                text = ('weather:%s' % error)[:48]
                log(text)
                return [frame(0xFE), frame(0xEE, text.encode('ascii', 'replace')),
                        frame(0xA4, bytes([0, 1]))]
        text = 'unsupported:%02X' % command
        log(text)
        return [frame(0xEE, text.encode('ascii'))]


def main():
    name = os.environ.get('MICROPY_UART_PIPE')
    log('start pid %d pipe %s reset %s' % (os.getpid(), name,
                                             os.environ.get('MICROPY_RESET_CAUSE')))
    if not name:
        log('MICROPY_UART_PIPE is not set')
        return 1
    pipe = open(name, 'r+b', buffering=0)
    model = Model()
    buffer = bytearray()
    while True:
        data = pipe.read(4096)
        if not data:
            log('pipe closed')
            return 0
        buffer += data
        while True:
            start = buffer.find(0x5A)
            if start < 0:
                buffer.clear()
                break
            del buffer[:start]
            if len(buffer) < 5:
                break
            length = buffer[2] | buffer[3] << 8
            if len(buffer) < 5 + length:
                break
            command = buffer[1]
            payload = bytes(buffer[4:4 + length])
            checksum = 0
            for byte in buffer[1:4 + length]:
                checksum ^= byte
            ok = checksum == buffer[4 + length]
            del buffer[:5 + length]
            if not ok:
                log('bad checksum for #%02X' % command)
                continue
            log('<- #%02X len %d' % (command, length))
            for reply in model.handle(command, payload):
                pipe.write(reply)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception:
        log(traceback.format_exc())
        sys.exit(1)
