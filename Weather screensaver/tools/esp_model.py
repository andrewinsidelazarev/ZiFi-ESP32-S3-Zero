"""Модель ESP32-S3 для стенда Unreal (ZiFi=UART) — на обычном Python.

Unreal создаёт именованный канал, кладёт его имя в переменную окружения
MICROPY_UART_PIPE и запускает эту программу; байты канала — это UART ZiFi.
Модель отвечает на команды заставки погоды так же, как прошивка:

  PING (#04)        -> READY (#F0);
  WIFI_INI (#03)    -> ACK, затем #83 [1][IPv4] и запоминает ключи ini;
  WEATHER_GET (#24) -> ACK, затем #A4 с записью погоды либо #EE + #A4 [0][1].

Погода настоящая: те же два HTTP-запроса, что делает прошивка
(api.zippopotam.us — индекс в координаты, api.open-meteo.com — прогноз), а
ответы разбирает сам код прошивки src/weather_parse.cpp, собранный host-тестом
в weather_parse_host.exe. Путь к нему модель берёт из esp_model.json рядом с
собой (его пишет tools/prepare_unreal.py). Остальные команды получают доклад
«unsupported», как в прошивке. Журнал — esp_model.log в каталоге запуска.
"""
import json
import os
import subprocess
import sys
import tempfile
import time
import traceback
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOG = HERE / 'esp_model.log'
CONFIG = json.loads((HERE / 'esp_model.json').read_text(encoding='utf-8'))
HOST_EXE = CONFIG['weather_parse_host']
USER_AGENT = 'ZiFi (ZX Evo)'
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
    не важен, ';' и '#' — комментарии, значение можно взять в кавычки."""
    result = {}
    text = data.decode('utf-8', 'replace').lstrip('﻿')
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


def http_get(host, path):
    request = urllib.request.Request('http://' + host + path, headers={
        'User-Agent': USER_AGENT, 'Accept': '*/*', 'Accept-Encoding': 'identity'})
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            return response.read()
    except urllib.error.HTTPError as error:
        raise RuntimeError('http %d' % error.code) from None


def run_host(*args):
    result = subprocess.run([HOST_EXE, *args], capture_output=True)
    output = result.stdout.decode('utf-8', 'replace').strip()
    if result.returncode != 0 or output.startswith('ERR'):
        raise RuntimeError(output[4:] if output.startswith('ERR ') else output)
    return output


class Model:
    def __init__(self):
        self.ini = {}
        self.coordinates = {}          # «страна/индекс» -> (широта, долгота, место)

    def weather(self):
        country = self.ini.get('country', '')
        zip_code = self.ini.get('zip', '')
        if not country or not zip_code:
            raise RuntimeError('no country/zip in ini')
        key = country + '/' + zip_code
        work = Path(tempfile.gettempdir())
        if key not in self.coordinates:
            try:
                body = http_get('api.zippopotam.us', '/' + urllib.request.quote(country) +
                                '/' + urllib.request.quote(zip_code))
            except Exception as error:
                raise RuntimeError('zip: %s' % error) from None
            source = work / 'esp_model_zip.json'
            source.write_bytes(body)
            geo = run_host('zip', str(source)).split(' ', 3)
            self.coordinates[key] = (float(geo[1]), float(geo[2]), geo[3])
            log('geo %s -> %s %s %s' % ((key,) + self.coordinates[key]))
        latitude, longitude, place = self.coordinates[key]
        try:
            body = http_get('api.open-meteo.com', FORECAST_PATH % (latitude, longitude))
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
