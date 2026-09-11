"""Машинный стенд заставок погоды: плагин .WMF исполняется в эмуляторе Z80.

Общее для обеих заставок (TS-Conf и VDAC2): подмена API Wild Commander
(файлы, страницы плагина, видеорежим, клавиатура), часы Mr.Gluk на портах
#xxF7 и модель ESP32-S3 на другом конце UART ZiFi (#xxEF). Что заставка
делает с экраном, проверяет её собственный тест: наследник Harness
добавляет свои порты (порты SPI FT812 у VDAC2) или функции WC (видеостраницы
TS-Conf) через port_in_extra / port_out_extra / api_extra.
"""
from __future__ import annotations

import datetime
import re
from pathlib import Path

import z80

WC_API = 0x6006
API_BREAK = WC_API + 5                  # RET заглушки WC_API
ALT_A_CELL = 0x7FF0                     # сюда заглушка кладёт A'
CODE_ADDRESS = 0x8000
STACK_ADDRESS = 0x7D00
RETURN_SENTINEL = 0x7F00
BREAKPOINT_HIT = 1 << 1
PAGE_SIZE = 16384
FRAME_MS = 20
REC_VERSION = 1                          # версия записи погоды (design.REC_VERSION)


class Symbols:
    """Адреса меток из .sym, который пишет sjasmplus (--sym)."""

    def __init__(self, path: Path):
        self.text = Path(path).read_text(encoding='utf-8', errors='replace')

    def __call__(self, name: str) -> int:
        match = re.search(rf'^{re.escape(name)}:\s+EQU\s+0x([0-9A-Fa-f]+)', self.text,
                          re.MULTILINE)
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
                reply += make_frame(0xA4, bytes([0, REC_VERSION]))
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

    def __init__(self, wmf_path: Path, sym_path: Path, esp: VirtualEsp,
                 start: datetime.datetime, key_frame: int, frame_ms: int = FRAME_MS):
        self.symbol = Symbols(sym_path)
        self.esp = esp
        self.time = start
        self.last_rtc = start               # время, которое Z80 прочитал последним
        self.key_frame = key_frame
        self.frame_ms = frame_ms
        self.frames = 0
        self.wmf = Wmf(Path(wmf_path).read_bytes())
        self.machine = z80.Z80Machine()
        self.memory = self.machine.memory
        # страницы данных плагина живут в памяти между запусками, как в WC:
        # всё, что записано в окно #0000, сохраняется при смене страницы
        self.data_pages = {n: bytearray(data) for n, data in self.wmf.pages.items() if n}
        self.data_page: int | None = None
        self.gedpl_pages: list[int | None] = []
        self.mode = None
        self.mode_history: list[int] = []
        self.bank = None
        self.bank_history: list[int] = []
        self.rtc_register = 0
        self.gate_writes = 0
        self.zifi_inits = 0                 # сколько раз плагин искал ZiFi
        self.snapshot_frames: set[int] = set()
        self.snapshots: dict[int, object] = {}
        self.api_calls: list[int] = []
        self.wait_release = 0
        self.ini = b'SSID: test\r\npassword: secret\r\ntime: +2\r\ncountry: IT\r\nzip: 00144\r\n'
        self.ini_present = True
        self.stream = 0
        self.found: dict[int, str | None] = {0: None, 1: None}
        self.cwd: dict[int, str] = {0: '/', 1: '/'}
        self.machine.set_memory_block(CODE_ADDRESS, self.wmf.pages[0])
        self.memory[self.symbol('Saver_Frame')] = 0xC9   # кадры считает тест
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
        value = self.port_in_extra(port)
        if value is None:
            raise AssertionError(f'чтение неизвестного порта #{port:04X}')
        return value

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
        if low == 0xEF:
            if high == 0xC7:
                if value == 0xF1:
                    self.zifi_inits += 1    # ZiFi_Init выбирает слой API 1
                return                      # команды очередей ZiFi
            if high == 0xBF:
                self.esp.feed_tx(value)
                return
        if not self.port_out_extra(port, value):
            raise AssertionError(f'запись в неизвестный порт #{port:04X}')

    def port_in_extra(self, port: int):
        """Порты заставки (наследник): значение или None — порт не наш."""
        return None

    def port_out_extra(self, port: int, value: int) -> bool:
        """Порты заставки (наследник): True — запись обработана."""
        return False

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
        if api == 78:                       # страница плагина -> #0000
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
            self.mode_history.append(self.mode)
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
        elif not self.api_extra(api):
            raise AssertionError(f'необработанный WC API {api} в PC=#{m.pc:04X}')
        self.ret()

    def api_extra(self, api: int) -> bool:
        """Функции WC, нужные только одной заставке (наследник)."""
        return False

    def alt_af_high(self) -> int:
        return self.memory[ALT_A_CELL]

    # --- запуск ---------------------------------------------------------------------------
    def snapshot(self):
        """Снимок экрана заставки для snapshot_frames (наследник)."""
        return None

    def on_frame(self) -> None:
        """Каждый кадр (наследник): например, отдать время чипу FT812."""

    def run(self, reason: int = 2, max_frames: int = 20000) -> None:
        m = self.machine
        m.a = reason
        m.ix = 0x7EA0
        m.sp = STACK_ADDRESS
        self.memory[STACK_ADDRESS] = RETURN_SENTINEL & 0xFF
        self.memory[STACK_ADDRESS + 1] = RETURN_SENTINEL >> 8
        m.pc = CODE_ADDRESS
        frame_hook = self.symbol('Saver_Frame')
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
                    self.snapshots[self.frames] = self.snapshot()
                self.frames += 1
                self.time += datetime.timedelta(milliseconds=self.frame_ms)
                self.esp.tick()
                self.on_frame()
                if self.frames > max_frames:
                    raise AssertionError('заставка не завершилась')
                self.ret()
            elif m.pc == RETURN_SENTINEL:
                return
            else:
                raise AssertionError(f'неожиданный breakpoint #{m.pc:04X}')
        raise AssertionError('превышен лимит шагов Z80')
