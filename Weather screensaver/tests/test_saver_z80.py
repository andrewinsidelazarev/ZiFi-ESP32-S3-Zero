"""Машинные тесты заставки: плагин исполняется в эмуляторе Z80 с подменой
API Wild Commander, портов часов Mr.Gluk и очередей ZiFi.

Кадр, нарисованный кодом Z80 в видеостраницах, сравнивается попиксельно с
эталоном tools/refscreen.py, который рисует тот же экран на Python по тем же
данным глифов, иконок и координат.
"""
from __future__ import annotations

import datetime
import re
import struct
import sys
import unittest
from pathlib import Path

import z80

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / 'tools'))
import design as D  # noqa: E402
import gen_assets  # noqa: E402
import refscreen  # noqa: E402

WMF = PROJECT / 'build' / 'WEATHER.WMF'
SYM = PROJECT / 'build' / 'WEATHER.sym'
WC_API = 0x6006
API_BREAK = WC_API + 5                  # RET заглушки WC_API
ALT_A_CELL = 0x7FF0                     # сюда заглушка кладёт A'
CODE_ADDRESS = 0x8000
STACK_ADDRESS = 0x7D00
RETURN_SENTINEL = 0x7F00
BREAKPOINT_HIT = 1 << 1
PAGE_SIZE = 16384
FRAME_MS = 20


def symbol(name: str) -> int:
    match = re.search(rf'^{re.escape(name)}:\s+EQU\s+0x([0-9A-Fa-f]+)', SYM.read_text(
        encoding='utf-8', errors='replace'), re.MULTILINE)
    if not match:
        raise AssertionError(f'символ {name} не найден')
    return int(match.group(1), 16)


def bcd(value: int) -> int:
    return (value // 10) * 16 + value % 10


class Wmf:
    """Разбор заголовка .WMF: код и страницы данных."""

    def __init__(self, data: bytes):
        assert data[16:32] == b'WildCommanderMDL'
        self.version = data[32]
        self.page_count = data[34]
        self.entry_page = data[35]
        self.blocks = [(data[36 + i * 2], data[37 + i * 2]) for i in range(6)]
        self.kind = data[197]
        self.name = data[165:197]
        self.pages: dict[int, bytes] = {}
        offset = 512
        for page, sectors in self.blocks:
            if sectors == 0:
                continue
            self.pages[page] = data[offset:offset + sectors * 512]
            offset += sectors * 512
        self.size = len(data)
        self.consumed = offset


def make_frame(command: int, payload: bytes = b'') -> bytes:
    """Пакет протокола ZiFi: SYNC, CMD, LEN, DATA, XOR-сумма от CMD."""
    body = bytes([command, len(payload) & 0xFF, len(payload) >> 8]) + payload
    checksum = 0
    for byte in body:
        checksum ^= byte
    return b'\x5A' + body + bytes([checksum])


class VirtualEsp:
    """Модель ESP на другом конце UART: разбирает команды, отвечает по плану.

    records — ответы на WEATHER_GET по очереди (None — ошибка); последний
    повторяется, если запросов больше."""

    def __init__(self, *, present=True, record=None, records=None, wifi_ok=True,
                 reply_delay=0, error_text=b''):
        self.present = present
        self.records = list(records) if records is not None else [record]
        self.weather_requests = 0
        self.request_frames: list[int] = []   # кадры, когда пришёл WEATHER_GET
        self.wifi_ok = wifi_ok
        self.reply_delay = reply_delay        # кадров до ответа WEATHER_GET
        self.error_text = error_text
        self.rx = bytearray()                 # очередь ESP -> Z80
        self.tx = bytearray()                 # байты Z80 -> ESP
        self.commands: list[tuple[int, bytes]] = []
        self.pending: list[tuple[int, bytes]] = []   # (кадр, байты)
        self.frame = 0

    def feed_tx(self, byte: int) -> None:
        self.tx.append(byte)
        while True:
            start = self.tx.find(b'\x5A')
            if start < 0:
                self.tx.clear()
                return
            if len(self.tx) < start + 5:
                return
            command = self.tx[start + 1]
            length = self.tx[start + 2] | self.tx[start + 3] << 8
            end = start + 4 + length + 1
            if len(self.tx) < end:
                return
            payload = bytes(self.tx[start + 4:start + 4 + length])
            del self.tx[:end]
            self.commands.append((command, payload))
            self.respond(command, payload)

    def respond(self, command: int, payload: bytes) -> None:
        if command == 0x04:
            self.rx += make_frame(0xF0)
        elif command == 0x03:
            self.rx += make_frame(0xFE)
            status = bytes([1, 192, 168, 1, 50]) if self.wifi_ok else bytes(5)
            self.rx += make_frame(0x83, status)
        elif command == 0x24:
            assert payload == b'', 'WEATHER_GET идёт без payload'
            self.rx += make_frame(0xFE)
            record = self.records[min(self.weather_requests, len(self.records) - 1)]
            self.weather_requests += 1
            self.request_frames.append(self.frame)
            reply = bytearray()
            if record is not None:
                reply += make_frame(0xA4, record)
            else:
                if self.error_text:
                    reply += make_frame(0xEE, self.error_text)
                reply += make_frame(0xA4, bytes([0, D.REC_VERSION]))
            self.pending.append((self.frame + self.reply_delay, bytes(reply)))
        else:
            raise AssertionError(f'неожиданная команда ESP #{command:02X}')

    def tick(self) -> None:
        self.frame += 1
        still = []
        for when, data in self.pending:
            if when <= self.frame:
                self.rx += data
            else:
                still.append((when, data))
        self.pending = still


class Harness:
    """Машина Z80 с плагином, подменой WC, часов и ZiFi."""

    def __init__(self, esp: VirtualEsp, start: datetime.datetime, key_frame: int,
                 frame_ms: int = FRAME_MS):
        self.esp = esp
        self.time = start
        self.last_rtc = start               # время, которое Z80 прочитал последним
        self.key_frame = key_frame
        self.frame_ms = frame_ms
        self.frames = 0
        self.wmf = Wmf(WMF.read_bytes())
        self.machine = z80.Z80Machine()
        self.memory = self.machine.memory
        self.video = [bytearray(PAGE_SIZE) for _ in range(16)]
        self.video_page: int | None = None
        # страницы данных плагина живут в памяти между запусками, как в WC:
        # всё, что записано в окно #0000, сохраняется при смене страницы
        self.data_pages = {n: bytearray(data) for n, data in self.wmf.pages.items() if n}
        self.data_page: int | None = None
        self.gedpl_pages: list[int | None] = []
        self.mode = None
        self.bank = None
        self.bank_history: list[int] = []
        self.fmaddr = 0
        self.fm_windows: list[int] = []
        self.cram = bytearray(512)
        self.rtc_register = 0
        self.gate_writes = 0
        self.zifi_inits = 0                 # сколько раз плагин искал ZiFi
        self.snapshot_frames: set[int] = set()
        self.snapshots: dict[int, bytes] = {}
        self.api_calls: list[int] = []
        self.wait_release = 0
        self.ini = b'SSID: test\r\npassword: secret\r\ntime: +2\r\ncountry: IT\r\nzip: 00144\r\n'
        self.ini_present = True
        self.stream = 0
        self.found: dict[int, str | None] = {0: None, 1: None}
        self.cwd: dict[int, str] = {0: '/', 1: '/'}
        self.machine.set_memory_block(CODE_ADDRESS, self.wmf.pages[0])
        self.memory[symbol('Saver_Frame')] = 0xC9   # кадры считает тест
        # Заглушка WC_API: сохраняет A' (параметр функции) в ALT_A_CELL и
        # останавливается на RET, где тест разбирает вызов и сам возвращается.
        self.machine.set_memory_block(WC_API, bytes([0x08, 0x32, ALT_A_CELL & 0xFF,
                                                    ALT_A_CELL >> 8, 0x08, 0xC9]))
        self.machine.set_input_callback(self.port_in)
        self.machine.set_output_callback(self.port_out)

    # --- порты --------------------------------------------------------------------------
    def port_in(self, port: int) -> int:
        low = port & 0xFF
        high = port >> 8
        if port == 0xBFF7:
            return self.rtc_value(self.rtc_register)
        if low == 0xEF:
            if high == 0xC7:
                return 0x01 if self.esp.present else 0xFF
            if high == 0xC0:
                return min(len(self.esp.rx), 0xFF) if self.esp.present else 0
            if high == 0xC1:
                return 0xFF
            if high < 0xC0:
                if not self.esp.rx:
                    return 0
                return self.esp.rx.pop(0)
        raise AssertionError(f'чтение неизвестного порта #{port:04X}')

    def port_out(self, port: int, value: int) -> None:
        low = port & 0xFF
        high = port >> 8
        if port == 0xDFF7:
            self.rtc_register = value
            return
        if port == 0xEFF7:
            assert value == 0x80, 'вентиль Mr.Gluk закрывать внутри WC нельзя'
            self.gate_writes += 1
            return
        if port == 0x15AF:
            # FMADDR (бит 4 — включить, биты 3..0 — A15..A12 окна) подключает
            # CRAM к окну. Модель z80 пишет в обычную RAM, поэтому при закрытии
            # окна палитра забирается из памяти под ним.
            if self.fmaddr & 0x10 and value == 0:
                base = (self.fmaddr & 0x0F) << 12
                self.cram[:] = self.memory[base:base + 512]
            if value:
                self.fm_windows.append(value)
            self.fmaddr = value
            return
        if low == 0xEF:
            if high == 0xC7:
                if value == 0xF1:
                    self.zifi_inits += 1    # ZiFi_Init выбирает слой API 1
                return                      # команды очередей ZiFi
            if high == 0xBF:
                self.esp.feed_tx(value)
                return
        raise AssertionError(f'запись в неизвестный порт #{port:04X}')

    def rtc_value(self, register: int) -> int:
        t = self.time
        if register == 0x00:
            self.last_rtc = t
        table = {0x00: t.second, 0x02: t.minute, 0x04: t.hour, 0x07: t.day,
                 0x08: t.month, 0x09: t.year % 100}
        if register in table:
            return bcd(table[register])
        if register == 0x0A:
            return 0                        # обновление не идёт
        if register == 0x0B:
            return 0x02                     # 24 часа, BCD
        raise AssertionError(f'чтение регистра часов #{register:02X}')

    # --- API Wild Commander ------------------------------------------------------------
    def flags(self, *, zero=False, carry=False) -> None:
        self.machine.f = (0x40 if zero else 0) | (1 if carry else 0)

    def ret(self) -> None:
        m = self.machine
        address = self.memory[m.sp] | self.memory[(m.sp + 1) & 0xFFFF] << 8
        m.sp = (m.sp + 2) & 0xFFFF
        m.pc = address

    def handle_api(self) -> None:
        m = self.machine
        api = m.a
        self.api_calls.append(api)
        if api == 65:                       # видеостраница -> #C000
            page = self.alt_af_high()
            assert 0 <= page < 16, page
            self.swap_video(page)
            self.flags(zero=True)
        elif api == 78:                     # страница плагина -> #0000
            page = self.alt_af_high()
            assert page in self.data_pages, page
            if self.data_page != page:
                if self.data_page is not None:
                    self.data_pages[self.data_page][:] = self.memory[0:PAGE_SIZE]
                self.data_page = page
                m.set_memory_block(0, self.data_pages[page])
            self.flags(zero=True)
        elif api == 66:
            self.mode = self.alt_af_high()
            self.flags(zero=True)
        elif api == 64:
            self.bank = self.alt_af_high()
            self.bank_history.append(self.bank)
            if self.bank == 0:
                # MNGV_PL с нулём вызывает GEDPL: палитра WC пишется через окно
                # FMADDR на #1000 и портит #1000..#1447 страницы в окне #0000.
                self.gedpl_pages.append(self.data_page)
                self.memory[0x1000:0x1020] = bytes([0xAA]) * 0x20
                self.memory[0x11E0:0x1400] = bytes(0x220)
                self.memory[0x1440:0x1448] = bytes(8)
            self.flags(zero=True)
        elif api == 45:
            self.flags(zero=self.frames < self.key_frame)
        elif api == 46:
            self.wait_release += 1
            self.flags(zero=True)
        elif api == 57:                     # STREAM
            if m.d == 0xFE:
                self.stream = 0
            elif m.d in (0, 1) and m.bc == 0xFFFF:
                self.stream = m.d
            elif m.d == 0xFF:
                self.cwd[self.stream] = '/'
            elif m.d in (0, 1):
                self.stream = m.d
                self.cwd[self.stream] = '/'
            self.flags(zero=True)
        elif api == 59:                     # FENTRY
            kind = self.memory[m.hl]
            name = bytes(self.memory[m.hl + 1:m.hl + 40]).split(b'\0')[0]
            path = self.cwd[self.stream].rstrip('/') + '/' + name.decode('ascii')
            if kind == 0x10 and path == '/zifi':
                self.found[self.stream] = path
                self.flags()
            elif kind == 0x00 and path == '/zifi/zifi.ini' and self.ini_present:
                self.found[self.stream] = path
                m.hl = len(self.ini)
                m.de = 0
                self.flags()
            else:
                self.flags(zero=True)
        elif api == 63:                     # GDIR
            self.cwd[self.stream] = self.found[self.stream] or '/'
            self.flags(zero=True)
        elif api == 62:                     # GFILE
            self.flags(zero=True)
        elif api == 48:                     # LOAD512
            assert m.b in (1, 2), f'LOAD512 на {m.b} секторов'
            data = self.ini[:m.b * 512].ljust(m.b * 512, b'\0')
            self.memory[m.hl:m.hl + len(data)] = data
            self.flags()
        else:
            raise AssertionError(f'необработанный WC API {api} в PC=#{m.pc:04X}')
        self.ret()

    def alt_af_high(self) -> int:
        return self.memory[ALT_A_CELL]

    def swap_video(self, page: int) -> None:
        if self.video_page is not None:
            self.video[self.video_page][:] = self.memory[0xC000:0x10000]
        self.video_page = page
        self.machine.set_memory_block(0xC000, self.video[page])

    # --- запуск ---------------------------------------------------------------------------
    def run(self, reason: int = 2, max_frames: int = 20000) -> None:
        m = self.machine
        m.a = reason
        m.ix = 0x7EA0
        m.sp = STACK_ADDRESS
        self.memory[STACK_ADDRESS] = RETURN_SENTINEL & 0xFF
        self.memory[STACK_ADDRESS + 1] = RETURN_SENTINEL >> 8
        m.pc = CODE_ADDRESS
        frame_hook = symbol('Saver_Frame')
        for address in (API_BREAK, RETURN_SENTINEL, frame_hook):
            m.set_breakpoint(address)
        for _ in range(2_000_000):
            m.ticks_to_stop = 50_000_000
            event = m.run()
            if not event & BREAKPOINT_HIT:
                continue
            if m.pc == API_BREAK:
                self.handle_api()
            elif m.pc == frame_hook:
                if self.frames in self.snapshot_frames:
                    self.snapshots[self.frames] = bytes(self.screen())
                self.frames += 1
                self.time += datetime.timedelta(milliseconds=self.frame_ms)
                self.esp.tick()
                if self.frames > max_frames:
                    raise AssertionError('заставка не завершилась')
                self.ret()
            elif m.pc == RETURN_SENTINEL:
                return
            else:
                raise AssertionError(f'неожиданный breakpoint #{m.pc:04X}')
        raise AssertionError('превышен лимит шагов Z80')

    def screen(self) -> bytearray:
        """Кадр 360x288 из видеостраниц банка 1."""
        if self.video_page is not None:
            self.video[self.video_page][:] = self.memory[0xC000:0x10000]
        out = bytearray()
        for y in range(D.SCREEN_H):
            page = self.video[y // 32]
            start = (y % 32) * 512
            out += page[start:start + D.SCREEN_W]
        return out

    def palette_words(self) -> list[int]:
        return [self.cram[i] | self.cram[i + 1] << 8 for i in range(0, 512, 2)]


def diff_count(a: bytes, b: bytes) -> int:
    return sum(1 for x, y in zip(a, b) if x != y)


def reference(assets, h: 'Harness', record, **kwargs):
    """Эталон кадра на время, которое плагин прочитал из часов последним."""
    t = h.last_rtc
    return refscreen.render(assets, t.year, t.month, t.day, t.hour, t.minute, record,
                            second=t.second, **kwargs)


def save_png(indices: bytes, path: Path) -> None:
    from PIL import Image
    assets = refscreen.Assets()
    img = Image.new('RGB', (D.SCREEN_W, D.SCREEN_H))
    img.putdata([assets.palette[i] for i in indices])
    img.save(path)


class SaverTests(unittest.TestCase):
    START = datetime.datetime(2026, 9, 10, 0, 25, 30)

    def setUp(self) -> None:
        self.assets = refscreen.Assets()

    def assert_frame(self, h: Harness, record, name: str, **kwargs) -> None:
        expected = reference(self.assets, h, record, **kwargs)
        actual = h.screen()
        save_png(actual, PROJECT / 'build' / f'test_{name}.png')
        mismatches = diff_count(actual, expected.buf)
        self.assertEqual(mismatches, 0, f'{mismatches} пикселей отличаются от эталона')

    def test_wmf_header(self) -> None:
        wmf = Wmf(WMF.read_bytes())
        self.assertEqual(wmf.version, 0x0A)
        self.assertEqual(wmf.kind, 0x02, 'тип заставки')
        self.assertEqual(wmf.page_count, 4)
        self.assertEqual(wmf.entry_page, 0)
        self.assertEqual([p for p, s in wmf.blocks if s], [0, 1, 2, 3])
        self.assertEqual(wmf.consumed, wmf.size)
        for page in (1, 2, 3):
            self.assertEqual(len(wmf.pages[page]), PAGE_SIZE)
            self.assertEqual(wmf.pages[page], (PROJECT / 'build' / f'page{page}.bin').read_bytes())
        self.assertTrue(wmf.name.startswith(b'ZiFi Weather Saver'))

    def test_full_screen_matches_reference(self) -> None:
        esp = VirtualEsp(record=refscreen.SAMPLE_RECORD)
        h = Harness(esp, self.START, key_frame=40)
        h.run()
        self.assertEqual(h.machine.a, 0)
        self.assertEqual(h.machine.ix, 0x7EA0)
        self.assertEqual(h.mode, 0xC2)
        self.assertEqual(h.bank_history, [1, 0], 'банк 1 на время показа, банк 0 при выходе')
        # при входе — чтобы Enter из F10 не закрыл заставку, при выходе —
        # чтобы закрывшая её клавиша не дошла до панелей
        self.assertEqual(h.wait_release, 2)
        self.assertEqual([c for c, _ in esp.commands], [0x04, 0x03, 0x24])
        self.assertEqual(esp.commands[1][1], h.ini)
        self.assertEqual(h.last_rtc.second % 2, 0)
        self.assert_frame(h, refscreen.SAMPLE_RECORD, 'full_screen')
        # палитра пишется через окно #C000 поверх видеостраницы, а не через
        # #0000, где лежит рабочая страница WC; цвета 240..255 текстового
        # экрана WC не трогаются
        self.assertEqual(h.fm_windows, [0x1C])
        self.assertEqual(h.gate_writes, 1, 'вентиль часов открывается один раз')
        self.assertEqual(h.palette_words()[:240], self.assets.meta['palette_cram'][:240])

    def test_second_run_after_gedpl_is_intact(self) -> None:
        # Страницы плагина живут в памяти WC между запусками. GEDPL при выходе
        # портит #1000..#1447 страницы в окне #0000; второй запуск обязан
        # рисовать тот же кадр (без подмены страницы пропадали буквы «ana»).
        esp = VirtualEsp(record=refscreen.SAMPLE_RECORD)
        h = Harness(esp, self.START, key_frame=40)
        h.run()
        self.assertEqual(h.gedpl_pages, [gen_assets.GEDPL_SAFE_PAGE])
        h.key_frame = h.frames + 40
        h.run()
        self.assertEqual(h.gedpl_pages, [gen_assets.GEDPL_SAFE_PAGE] * 2)
        self.assertEqual(esp.weather_requests, 2)
        self.assert_frame(h, refscreen.SAMPLE_RECORD, 'second_run')

    def test_colon_blinks_every_second(self) -> None:
        esp = VirtualEsp(record=refscreen.SAMPLE_RECORD)
        h = Harness(esp, self.START, key_frame=60)   # 1,2 с: секунда 31
        h.run()
        self.assertEqual(h.last_rtc.second % 2, 1)
        self.assert_frame(h, refscreen.SAMPLE_RECORD, 'colon_hidden')

    def test_without_zifi_clock_and_status(self) -> None:
        esp = VirtualEsp(present=False)
        h = Harness(esp, self.START, key_frame=10)
        h.run()
        self.assertEqual(esp.commands, [])
        self.assert_frame(h, None, 'no_zifi', status=D.encode_cp866(D.STRINGS['NO_ZIFI']))

    def test_long_ini_is_sent_whole(self) -> None:
        # Пример zifi.ini с комментариями длиннее сектора; country:/zip: в конце
        # обязаны дойти до ESP.
        esp = VirtualEsp(record=refscreen.SAMPLE_RECORD)
        h = Harness(esp, self.START, key_frame=40)
        # строки короче 160 символов: длиннее не принимает разбор ini в ESP
        comments = b''.join(b'; comment line %02d of the example zifi.ini file\r\n' % i
                            for i in range(12))
        h.ini = comments + b'SSID: test\r\npassword: secret\r\ncountry: IT\r\nzip: 00144\r\n'
        self.assertGreater(len(h.ini), 512)
        h.run()
        self.assertEqual(esp.commands[1], (0x03, h.ini))
        self.assert_frame(h, refscreen.SAMPLE_RECORD, 'long_ini')

    def test_too_long_ini_is_reported(self) -> None:
        esp = VirtualEsp(record=refscreen.SAMPLE_RECORD)
        h = Harness(esp, self.START, key_frame=10)
        h.ini = b'; ' + b'x' * 1100 + b'\r\ncountry: IT\r\nzip: 00144\r\n'
        h.run()
        self.assertEqual([c for c, _ in esp.commands], [0x04], 'без ini в сеть не ходим')
        self.assert_frame(h, None, 'ini_too_long',
                          status=D.encode_cp866(D.STRINGS['INI_TOO_LONG']))

    def test_esp_error_text_is_shown(self) -> None:
        esp = VirtualEsp(record=None, error_text=b'weather:zip: http 404')
        h = Harness(esp, self.START, key_frame=10)
        h.run()
        self.assert_frame(h, None, 'esp_error', status=D.encode_cp866(D.STRINGS['NO_WEATHER']),
                          error=b'weather:zip: http 404')

    def test_clock_advances_while_esp_is_slow(self) -> None:
        # Ответ приходит через 60 кадров; за это время минута сменилась —
        # часы обязаны перерисоваться, пока плагин ждёт ESP.
        start = datetime.datetime(2026, 9, 10, 0, 25, 59)
        esp = VirtualEsp(record=refscreen.SAMPLE_RECORD, reply_delay=60)
        h = Harness(esp, start, key_frame=80)
        h.run()
        self.assertEqual(h.last_rtc.minute, 26)
        self.assert_frame(h, refscreen.SAMPLE_RECORD, 'slow_esp')

    def test_key_during_wait_exits_immediately(self) -> None:
        esp = VirtualEsp(record=refscreen.SAMPLE_RECORD, reply_delay=500)
        h = Harness(esp, self.START, key_frame=5)
        h.run()
        self.assertLess(h.frames, 20)
        self.assertEqual(h.bank_history[-1], 0)
        self.assertEqual(h.wait_release, 2)

    def test_day_change_redraws_calendar_and_refreshes(self) -> None:
        start = datetime.datetime(2026, 9, 30, 23, 59, 59)
        esp = VirtualEsp(record=refscreen.SAMPLE_RECORD)
        h = Harness(esp, start, key_frame=120)
        h.run()
        self.assertEqual((h.last_rtc.month, h.last_rtc.day), (10, 1))
        self.assertEqual(esp.weather_requests, 2, 'новый день — новый прогноз')
        self.assert_frame(h, refscreen.SAMPLE_RECORD, 'new_day')

    def test_weather_refresh_every_hour(self) -> None:
        # Один кадр — минута модельного времени: запрос при запуске, затем
        # через 60 и 120 минут.
        start = datetime.datetime(2026, 9, 10, 10, 0, 0)
        esp = VirtualEsp(record=refscreen.SAMPLE_RECORD)
        h = Harness(esp, start, key_frame=150, frame_ms=60_000)
        h.run()
        self.assertEqual(esp.weather_requests, 3)
        self.assertEqual([c for c, _ in esp.commands].count(0x03), 1, 'Wi-Fi поднимается один раз')
        self.assert_frame(h, refscreen.SAMPLE_RECORD, 'hourly')

    def test_failed_refresh_keeps_shown_weather(self) -> None:
        # Обновление через час не удалось (и повторы тоже) — на экране
        # остаётся прежняя погода, без надписи об ошибке.
        start = datetime.datetime(2026, 9, 10, 10, 0, 0)
        esp = VirtualEsp(records=[refscreen.SAMPLE_RECORD, None], error_text=b'weather:no wifi')
        h = Harness(esp, start, key_frame=80, frame_ms=60_000)
        h.run()
        self.assertGreaterEqual(esp.weather_requests, 3, 'после неудачи — повтор')
        self.assert_frame(h, refscreen.SAMPLE_RECORD, 'refresh_failed')

    def test_retry_after_503_clears_error(self) -> None:
        # Первый ответ — 503: на экране причина. Через 10 секунд повтор:
        # надпись стирается («Запрос погоды…»), затем приходит погода.
        # ESP отвечает через полсекунды — видно, что на экране во время повтора
        esp = VirtualEsp(records=[None, refscreen.SAMPLE_RECORD],
                         error_text=b'weather:meteo: http 503', reply_delay=25)
        h = Harness(esp, self.START, key_frame=700)
        h.snapshot_frames = {300} | set(range(450, 650))
        h.run()
        self.assertEqual(esp.weather_requests, 2)
        gap_seconds = (esp.request_frames[1] - esp.request_frames[0]) * FRAME_MS / 1000
        self.assertAlmostEqual(gap_seconds, 10, delta=1.5)
        t = h.last_rtc
        box = self.assets.meta['clock']['box']

        def mismatch_outside_clock(shot, frame):
            # двоеточие мигает — строки часов не сравниваем
            return sum(1 for i, (a, b) in enumerate(zip(shot, frame.buf))
                       if a != b and not (box[1] <= i // D.SCREEN_W < box[1] + box[3]))

        # кадр 300 (6 с после старта): причина неудачи на экране
        error_frame = refscreen.render(self.assets, t.year, t.month, t.day, 0, 25, None,
                                       status=D.encode_cp866(D.STRINGS['NO_WEATHER']),
                                       error=b'weather:meteo: http 503')
        self.assertEqual(mismatch_outside_clock(h.snapshots[300], error_frame), 0,
                         'до повтора видна причина неудачи')
        # идёт повтор: причина стёрта, вместо неё «Запрос погоды…»
        loading_frame = refscreen.render(self.assets, t.year, t.month, t.day, 0, 25, None,
                                         status=D.encode_cp866(D.STRINGS['LOADING']))
        during = h.snapshots[esp.request_frames[1] + 10]
        self.assertEqual(mismatch_outside_clock(during, loading_frame), 0,
                         'при повторе причина стёрта')
        self.assert_frame(h, refscreen.SAMPLE_RECORD, 'retry_ok')

    def test_retry_schedule_after_failures(self) -> None:
        # Сплошные неудачи: паузы 10, 20, 30, 60, 120, 300, 300 секунд.
        esp = VirtualEsp(records=[None], error_text=b'weather:meteo: http 503')
        h = Harness(esp, self.START, key_frame=1000, frame_ms=1000)
        h.run()
        frames = esp.request_frames
        gaps = [b - a for a, b in zip(frames, frames[1:])]
        expected = [10, 20, 30, 60, 120, 300]
        self.assertGreaterEqual(len(gaps), len(expected), gaps)
        for gap, want in zip(gaps, expected):
            self.assertAlmostEqual(gap, want, delta=3, msg=f'паузы {gaps}')
        self.assertTrue(all(abs(g - 300) <= 3 for g in gaps[len(expected):]), gaps)

    def test_no_retry_without_zifi(self) -> None:
        esp = VirtualEsp(present=False)
        h = Harness(esp, self.START, key_frame=400, frame_ms=1000)
        h.run()
        self.assertEqual(h.zifi_inits, 1, 'без ZiFi повторять нечего')

    def test_description_on_calendar_title_baseline(self) -> None:
        # «Гроза» слева и «СЕНТЯБРЬ 2026» справа — на одной базовой линии:
        # низ буквы без выносных элементов («Г», «Е») — в одной строке экрана.
        L = D.LAYOUT
        body, caps = self.assets.font('BODY'), self.assets.font('CAPS')
        g = body.glyph(D.encode_cp866('Г')[0])
        e = caps.glyph(D.encode_cp866('Е')[0])
        self.assertEqual(L['DESC_Y'] + g.top + g.h, L['CAL_TITLE_Y'] + e.top + e.h)
        self.assertEqual(L['DESC_Y'] + body.ascent, L['CAL_TITLE_Y'] + caps.ascent)

    def test_calendar_disc_centred_on_number(self) -> None:
        # Центр круга сегодняшнего дня — ровно в центре рисунка цифр по обеим
        # осям: и у чисел чётной ширины («10» — 12 точек), и у нечётной
        # («21» — 13). Круг меряется прямо по кадру эталона (с кадром Z80 он
        # совпадает попиксельно — это проверяют тесты выше).
        L = D.LAYOUT
        top = self.assets.meta['disc']['y']
        base = D.PAL_TEXT0 + D.TEXT_SET_INDEX['accent_strip'] * 3
        dark = {D.PAL_TEXT0 + D.TEXT_SET_INDEX['dark_accent'] * 3 + k for k in range(3)}
        for day in range(1, 31):
            s = refscreen.render(self.assets, 2026, 9, day, 12, 0, None)
            x0 = L['CAL_COL_X0'] + refscreen.weekday(2026, 9, day) * L['CAL_COL_W']
            digits = []
            mass = sum_x = sum_y = 0
            for y in range(top - 1, top + L['CAL_CIRCLE_D'] + 1):
                for x in range(x0 - 1, x0 + L['CAL_COL_W'] + 1):
                    color = s.buf[y * D.SCREEN_W + x]
                    if color in dark:
                        digits.append((x, y))
                        level = 3       # цифры лежат там, где круг сплошной
                    elif base <= color < base + 3:
                        level = color - base + 1
                    else:
                        continue
                    # центр масс круга: вес точки — уровень её покрытия
                    mass += level
                    sum_x += level * (x + 0.5)
                    sum_y += level * (y + 0.5)
            digit_x = (min(x for x, _ in digits) + max(x for x, _ in digits) + 1) / 2
            digit_y = (min(y for _, y in digits) + max(y for _, y in digits) + 1) / 2
            self.assertAlmostEqual(sum_x / mass, digit_x, delta=0.01, msg=f'{day}: по горизонтали')
            self.assertAlmostEqual(sum_y / mass, digit_y, delta=0.01, msg=f'{day}: по вертикали')


if __name__ == '__main__':
    unittest.main()
