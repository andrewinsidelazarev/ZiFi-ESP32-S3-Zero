"""Машинные тесты плагина ZiFi WC Update (WCUPDATE.WMF) в эмуляторе Z80.

Стенд моделирует то, от чего зависит правильность плагина:
- окно #C000 со страницами: таблица списка живёт в странице 2 плагина, экран
  WC — в TXT (API 0 с #FE). PRSRW пишет прямо в #C000, поэтому печать без
  подключённого TXT — ошибка; строка таблицы, записанная не в страницу 2,
  тесту просто не видна;
- экран окна: каждая пара PRSRW/PRIAT ложится в модель строк с цветом;
- клавиши WC и сценарий ESP: события, ответы и файловые запросы.
Проверяется: разбор событий STATE/ENTRY в таблицу, вывод строк (отметка,
размеры, состояние, хвост длинного пути, курсор, полоса хода), рисование по
одной строке за шаг и только при пустой очереди ZiFi, Space/A/Enter без
потери коротких нажатий, повтор команды без ACK с обслуживанием файловых
запросов, повтор списка (SYNC) с меткой и проверкой полноты, RENAME через
API 74, OPEN на запись только свободного имени и выход со STOP.
Нужна сборка: "WC Update\\build.bat".
"""
import pathlib
import subprocess
import unittest

import z80

from test_ftp_plugin_z80 import frame, le16

ROOT = pathlib.Path(__file__).resolve().parents[1]
WMF = ROOT / "WC Update" / "build" / "WCUPDATE.WMF"
SYM = ROOT / "WC Update" / "build" / "WCUPDATE.sym"

WC_API = 0x6006
WC_FRAMES = 0x6009
WC_HEI = 0x600E
API_STUB = 0x7FE0
API_BREAK = API_STUB + 5
ALT_A_CELL = 0x7FF0
CODE = 0x8000
STACK = 0x5F00
SENTINEL = 0x7F00
INT_HANDLER = 0x0038
BREAKPOINT_HIT = 1 << 1
TXT = "txt"

FN_MNGC_PL, FN_PRWOW, FN_RRESB, FN_PRSRW, FN_PRIAT, FN_TXTPR = 0, 1, 2, 3, 4, 11
FN_GEDPL, FN_ESC, FN_KBSCN, FN_WAITUP, FN_TURBOPL = 15, 23, 42, 46, 14
FN_TENTRY, FN_STREAM, FN_FENTRY, FN_GFILE, FN_GDIR = 51, 57, 59, 62, 63
FN_MKFILE, FN_MKDIR, FN_RENAME, FN_DELETE, FN_FILEX, FN_INT_PL = 72, 73, 74, 75, 77, 86
KEY_API = {16: "space", 17: "up", 18: "down", 22: "enter", 23: "esc",
           25: "pgup", 26: "pgdn", 27: "home", 28: "end"}

CMD_WCU_START, CMD_WCU_APPLY, CMD_WCU_STOP, CMD_WCU_SYNC = 0x25, 0x26, 0x27, 0x28
EVT_WCU_STATE, EVT_WCU_ENTRY = 0x67, 0x68
RESP_WCU_START, RESP_WCU_APPLY, RESP_WCU_STOP, RESP_WCU_SYNC = 0xA5, 0xA6, 0xA7, 0xA8
RESP_ACK = 0xFE
VFS_OPEN, VFS_MKDIR, VFS_RENAME, VFS_MOVE_RENAME = 0x50, 0x55, 0x59, 0x5D
FILEX_CAP_MOVE_RENAME = 0x10

SAME, DIFF, NEW, LOCAL_ONLY, KEPT, KEPT_DIFF, READ_ERR, UPDATED, FAILED = range(1, 10)
PH_CHECK, PH_READY, PH_APPLY, PH_SYNC, PH_ERROR = 3, 4, 5, 6, 0xFF
F_REMOTE, F_LOCAL, F_KEPT, F_CAN, F_AUTO, F_MARK = 1, 2, 4, 8, 16, 128

UI_COLOR, UI_CURSOR, UI_WARN, UI_BAD, UI_GOOD = 0x17, 0x50, 0x1E, 0x1A, 0x1C
ENTRY_SIZE = 64
STATUS_LINE, BAR_LINE, HEAD_LINE = 1, 2, 3
LIST_LINE = 4                            # первая строка списка в окне
LIST_ROWS = 14                           # окно 19 строк с рамкой, как у FTP

# Оценка тактов вызовов WC по его исходникам (с запасом): вход диспетчера
# FUN с переходником, PRSRW — LDIR строки, PRIAT — цикл по байтам атрибутов.
API_TICKS = {"dispatch": 170, FN_MNGC_PL: 100}
PRSRW_TICKS = (400, 21)                  # постоянная часть, на символ
PRIAT_TICKS = (150, 24)
TXTPR_TICKS = (300, 150)                 # на выведенный знак, с атрибутом
OTHER_API_TICKS = 300
# Очередь приёма ZiFi — 256 байт: на 115200 бод это 22 мс, при 3,5 МГц —
# 77 700 тактов. Один шаг рисования обязан уложиться в половину.
STEP_BUDGET = 38_000
# UART 115200 8N1 — 11 520 байт/с. Плагин работает на 14 МГц (TURBOPL):
# байт приходит раз в 1215 тактов, кадр — 280 000 тактов.
CPU_HZ = 14_000_000
BYTE_TICKS = CPU_HZ / 11_520
FRAME_TICKS = CPU_HZ // 50
ZIFI_FIFO = 256


def ticks_left(machine) -> int:
    """Остаток тактов прогона. Свойство ticks_to_stop пакета z80 читает
    старший байт не оттуда (image[2] вместо image[3]), поэтому поле
    разбирается здесь."""
    return int.from_bytes(bytes(machine._StateBase__ticks_to_stop), "little")


def state(phase: int, text: bytes, percent: int = 0, cur: int = 0, tot: int = 0) -> bytes:
    return frame(EVT_WCU_STATE, bytes([phase]) + le16(cur) + le16(tot) +
                 bytes([percent]) + text)


def entry(index: int, status: int, flags: int, local: int, remote: int,
          path: bytes) -> bytes:
    return frame(EVT_WCU_ENTRY, bytes([index, status, flags]) +
                 local.to_bytes(3, "little") + remote.to_bytes(3, "little") + path)


def generation(rows: list[bytes], last: bytes) -> bytes:
    """Ответ ESP на SYNC: метка (этап 6, всего — число строк), строки по
    порядку номеров и последнее состояние."""
    return state(PH_SYNC, b"", 0, 0, len(rows)) + b"".join(rows) + last


def build_is_stale() -> bool:
    """Сборка старше своих исходников: тесты проверяли бы прежний код."""
    built = min(WMF.stat().st_mtime, SYM.stat().st_mtime)
    sources = list((ROOT / "WC Update" / "src").glob("*"))
    sources += list((ROOT / "shared" / "z80").glob("*"))
    sources += [ROOT / "FTP Server" / "src" / "vfs.asm",
                ROOT / "FTP Server" / "src" / "fs.asm"]
    return any(path.stat().st_mtime > built for path in sources if path.is_file())


_BUILD: list[str] = []


def require_build() -> None:
    """Нет сборки (в комплекте лежит только WMF) или исходники новее —
    собрать build.bat. Нет ассемблера — пропуск; сборка упала — ошибка."""
    if not _BUILD:
        state = "ok"
        if not WMF.exists() or not SYM.exists() or build_is_stale():
            result = subprocess.run(["cmd", "/c", str(ROOT / "WC Update" / "build.bat")],
                                    capture_output=True)
            output = (result.stdout + result.stderr).decode("utf-8", "replace")
            if "sjasmplus.exe was not found" in output:
                state = "skip"
            elif result.returncode != 0 or not WMF.exists() or not SYM.exists():
                state = "fail:" + output
        _BUILD.append(state)
    if _BUILD[0] == "skip":
        raise unittest.SkipTest("нет sjasmplus: WC Update/build не собрать")
    if _BUILD[0].startswith("fail:"):
        raise RuntimeError("сборка WC Update не удалась:\n" + _BUILD[0][5:])


class Symbols:
    def __init__(self, path: pathlib.Path) -> None:
        self.text = path.read_text(encoding="utf-8", errors="replace")
        self.cache: dict[str, int] = {}

    def __call__(self, name: str) -> int:
        if name not in self.cache:
            import re
            match = re.search(rf"^{re.escape(name)}:\s+EQU\s+0x([0-9A-Fa-f]+)",
                              self.text, re.M)
            if not match:
                raise AssertionError(f"в WCUPDATE.sym нет символа {name}")
            self.cache[name] = int(match.group(1), 16)
        return self.cache[name]


class Harness:
    """Плагин в памяти Z80, WC и ESP — модели."""

    def __init__(self) -> None:
        data = WMF.read_bytes()
        assert data[16:32] == b"WildCommanderMDL"
        sectors = data[37]
        self.symbol = Symbols(SYM)
        self.machine = z80.Z80Machine()
        self.memory = self.machine.memory
        self.machine.set_memory_block(CODE, data[512:512 + sectors * 512])
        self.machine.set_memory_block(WC_API, bytes([0xC3, API_STUB & 0xFF, API_STUB >> 8]))
        self.machine.set_memory_block(
            API_STUB, bytes([0x08, 0x32, ALT_A_CELL & 0xFF, ALT_A_CELL >> 8, 0x08, 0xC9]))
        self.machine.set_memory_block(INT_HANDLER, bytes([0xFB, 0xC9]))
        self.memory[WC_HEI] = 25
        self.machine.set_input_callback(self.port_in)
        self.machine.set_output_callback(self.port_out)
        for address in (API_BREAK, SENTINEL):
            self.machine.set_breakpoint(address)
        # Страницы окна #C000: пока подключена одна, её содержимое — в памяти.
        self.banks = {TXT: bytearray(0x4000), 0: bytearray(0x4000),
                      1: bytearray(0x4000), 2: bytearray(0x4000)}
        self.mapped = TXT
        self.calls: list[int] = []
        self.tx = bytearray()
        self.rx = bytearray()
        self.rx_polls = 0
        self.esp = None
        self.pressed: set[str] = set()   # клавиши: каждая срабатывает один раз
        self.kbscn: int | None = None
        self.screen: dict[int, bytearray] = {}
        self.colors: dict[int, list[int]] = {}
        self.prints: list[tuple[int, int, bytes]] = []
        self.last_print = None
        self.ticks = 0                          # такты Z80 и оценка вызовов WC
        self.segment_start = 0                  # остаток тактов в начале прогона
        # Линия ESP -> Z80 с настоящими временем и очередью: байты wire
        # приходят в rx по одному раз в BYTE_TICKS, очередь ZiFi — 256 байт,
        # не влезший байт теряется, как в железе.
        self.wire = bytearray()
        self.wire_clock = 0.0
        self.fifo_peak = 0
        self.lost = 0
        self.turbo: list[tuple[int, int]] = []  # (B, C) вызовов TURBOPL
        self.renames: list[tuple[bytes, bytes]] = []
        # Каталоги: имя -> кластер из записи (+20 — старшее слово, +26 — младшее).
        # У WC старшие 4 бита +20 — не номер кластера, стенд их ставит нарочно.
        self.dirs = {b"WC": 0xF0123456, b"MENU": 0x00000789}
        self.last_dir: bytes | None = None
        self.filex_caps = 0xFF                  # FILEX WC Improved: все операции
        self.filex_status = 0
        self.moves: list[dict] = []
        self.rename_code: int | None = None     # None — RENAME удался
        self.files: set[bytes] = set()          # файлы каталога для FENTRY/MKFILE
        self.mkfiles: list[bytes] = []
        self.interrupts = 0

    # --- страницы -----------------------------------------------------------------
    def map_c000(self, page) -> None:
        if page == self.mapped:
            return
        self.banks[self.mapped][:] = self.memory[0xC000:0x10000]
        self.memory[0xC000:0x10000] = self.banks[page]
        self.mapped = page

    def page(self, number) -> bytes:
        if number == self.mapped:
            return bytes(self.memory[0xC000:0x10000])
        return bytes(self.banks[number])

    def table_entry(self, index: int) -> bytes:
        return self.page(2)[index * ENTRY_SIZE:(index + 1) * ENTRY_SIZE]

    # --- ZiFi ---------------------------------------------------------------------
    def port_in(self, port: int) -> int:
        if port & 0xFF == 0xEF:
            high = port >> 8
            if high == 0xC7:
                return 0x01
            if high == 0xC0:
                self.rx_polls += 1
                if self.esp is not None:
                    self.esp(self)
                self.pump()
                return min(len(self.rx), 0xBF)
            if high == 0xC1:
                return 0xFF
            if high < 0xC0 and self.rx:
                return self.rx.pop(0)
            return 0
        return 0xFF

    def now(self) -> float:
        """Такты с начала работы стенда, в том числе внутри прогона."""
        return self.ticks + (self.segment_start - ticks_left(self.machine))

    def send(self, data: bytes) -> None:
        """ESP начинает слать байты по линии."""
        if not self.wire:
            self.wire_clock = self.now()
        self.wire += data

    def pump(self) -> None:
        if not self.wire:
            return
        arrived = int((self.now() - self.wire_clock) // BYTE_TICKS)
        if arrived <= 0:
            return
        arrived = min(arrived, len(self.wire))
        room = ZIFI_FIFO - len(self.rx)
        self.rx += self.wire[:min(arrived, room)]
        self.lost += max(0, arrived - room)
        del self.wire[:arrived]
        self.wire_clock += arrived * BYTE_TICKS
        self.fifo_peak = max(self.fifo_peak, len(self.rx))

    def port_out(self, port: int, value: int) -> None:
        if port == 0xBFEF:
            self.tx.append(value)

    def frames(self) -> list[tuple[int, bytes]]:
        frames, data, index = [], bytes(self.tx), 0
        while index < len(data):
            assert data[index] == 0x5A, "кадр должен начинаться с SYNC"
            command = data[index + 1]
            length = data[index + 2] | data[index + 3] << 8
            payload = data[index + 4:index + 4 + length]
            check = command ^ data[index + 2] ^ data[index + 3]
            for byte in payload:
                check ^= byte
            assert data[index + 4 + length] == check, "контрольная сумма кадра"
            frames.append((command, payload))
            index += 5 + length
        return frames

    def commands(self) -> list[int]:
        return [command for command, _ in self.frames()]

    def next_frame(self) -> None:
        value = (self.word(WC_FRAMES) + 1) & 0xFFFF
        self.memory[WC_FRAMES] = value & 0xFF
        self.memory[WC_FRAMES + 1] = value >> 8

    def word(self, address: int) -> int:
        return self.memory[address] | self.memory[address + 1] << 8

    def byte(self, name: str) -> int:
        return self.memory[self.symbol(name)]

    def text(self, name: str, length: int) -> bytes:
        address = self.symbol(name)
        return bytes(self.memory[address:address + length])

    # --- WC API -------------------------------------------------------------------
    def flags(self, *, zero: bool = False, carry: bool = False) -> None:
        self.machine.f = (0x40 if zero else 0) | (1 if carry else 0)

    def ret(self) -> None:
        m = self.machine
        address = self.memory[m.sp] | self.memory[(m.sp + 1) & 0xFFFF] << 8
        m.sp = (m.sp + 2) & 0xFFFF
        m.pc = address

    def handle_api(self) -> None:
        m = self.machine
        api = m.a
        param = self.memory[ALT_A_CELL]
        self.calls.append(api)
        self.ticks += API_TICKS["dispatch"]
        if api == FN_PRSRW:
            self.ticks += PRSRW_TICKS[0] + PRSRW_TICKS[1] * m.bc
        elif api == FN_PRIAT:
            self.ticks += PRIAT_TICKS[0] + PRIAT_TICKS[1] * m.b
        elif api == FN_TXTPR:
            pass                                # считает сам txtpr()
        else:
            self.ticks += API_TICKS.get(api, OTHER_API_TICKS)
        if api == FN_MNGC_PL:
            self.map_c000(TXT if param == 0xFE else param)
        elif api == FN_PRSRW:
            self.prsrw(m)
        elif api == FN_PRIAT:
            self.priat(m, param)
        elif api == FN_TXTPR:
            self.txtpr(m)
        elif api in KEY_API:
            key = KEY_API[api]
            pressed = key in self.pressed
            self.pressed.discard(key)
            self.flags(zero=not pressed)
        elif api == FN_KBSCN:
            assert param == 1, "букву A читать по сырой таблице (любая раскладка)"
            if self.kbscn is None:
                m.a = 0
                self.flags(zero=True)
            else:
                m.a = self.kbscn
                self.kbscn = None
                self.flags()
        elif api == FN_TURBOPL:
            self.turbo.append((m.b, m.c))
        elif api == FN_PRWOW:
            # PRWOW подключает TXT и оставляет его в #C000.
            self.map_c000(TXT)
            self.flags(zero=True)
        elif api in (FN_RRESB, FN_GEDPL, FN_INT_PL, FN_WAITUP, FN_STREAM, FN_GDIR,
                     FN_GFILE):
            self.flags(zero=True)
        elif api == FN_FENTRY:
            kind = self.memory[m.hl]
            name = bytes(self.memory[m.hl + 1:m.hl + 257]).split(bytes(1), 1)[0]
            found = (kind == 0x10 and name in self.dirs) or \
                (kind == 0 and name in self.files)
            self.last_dir = name if found else None
            self.flags(zero=not found)
        elif api == FN_TENTRY:
            assert self.last_dir is not None, "TENTRY без найденной записи"
            cluster = self.dirs[self.last_dir]
            record = bytearray(32)
            record[20:22] = (cluster >> 16).to_bytes(2, "little")
            record[26:28] = (cluster & 0xFFFF).to_bytes(2, "little")
            self.memory[m.de:m.de + 32] = record
        elif api == FN_RENAME:
            kind = self.memory[m.hl]
            name = bytes(self.memory[m.hl + 1:m.hl + 257]).split(bytes(1), 1)[0]
            old = bytes([kind]) + name
            new = bytes(self.memory[m.de:m.de + 256]).split(bytes(1), 1)[0]
            self.renames.append((old, new))
            if self.rename_code is None:
                self.flags()                    # NZ — переименовано
            else:
                m.a = self.rename_code
                self.flags(zero=True)           # Z и код WC в A
        elif api == FN_MKDIR:
            self.flags(zero=True)               # Z — каталог создан
        elif api == FN_MKFILE:
            # [атрибут][размер 4][имя,0]: занятое имя WC не создаёт (A=4, NZ).
            name = bytes(self.memory[m.hl + 5:m.hl + 261]).split(bytes(1), 1)[0]
            self.mkfiles.append(name)
            if name in self.files:
                m.a = 4
                self.flags()
            else:
                self.files.add(name)
                m.hl = 0
                m.de = 0
                self.flags(zero=True)
        elif api == FN_FILEX:
            block = m.hl
            assert 0x8000 <= block <= 0xC000 - 32, "блок FILEX — в окне #8000"
            operation = self.memory[block + 2]
            status = 0
            if operation == 0:
                self.memory[block + 24] = self.filex_caps
                self.memory[block + 25] = 0
                self.memory[block + 29] = 1
            elif operation == 5:
                def text(pointer: int, length: int) -> bytes:
                    assert 0x8000 <= pointer and pointer + length <= 0xC000
                    return bytes(self.memory[pointer:pointer + length])
                self.moves.append({
                    "flags": self.memory[block + 3],
                    "source": text(self.word(block + 8), self.word(block + 10)),
                    "dest": text(self.word(block + 12), self.word(block + 14)),
                    "source_dir": int.from_bytes(self.memory[block + 16:block + 20], "little"),
                    "dest_dir": int.from_bytes(self.memory[block + 20:block + 24], "little"),
                })
                status = self.filex_status
            else:
                raise AssertionError(f"неожиданная операция FILEX {operation}")
            self.memory[block + 28] = status
            m.a = status
            self.flags(zero=status == 0)
        else:
            raise AssertionError(f"неожиданный вызов WC API {api} в PC=#{m.pc:04X}")
        self.ret()

    def prsrw(self, m) -> None:
        assert self.mapped == TXT, "PRSRW без TXT в #C000: строка легла бы не на экран"
        assert not self.rx and self.memory[self.symbol("ProtoBurstLeft")] == 0, \
            "рисование при непрочитанных байтах ZiFi: очередь может переполниться"
        line, column, length = m.d, m.e, m.bc
        assert m.hl + length <= 0xC000, "текст строки читается из окна #C000"
        text = bytes(self.memory[m.hl:m.hl + length])
        row = self.screen.setdefault(line, bytearray(b" " * 80))
        row[column - 1:column - 1 + length] = text
        self.prints.append((line, column, text))
        # WC: HL — экранный адрес за текстом, B — длина; их берёт PRIAT.
        m.hl = 0xC000 + line * 256 + column + length
        m.b = length & 0xFF
        self.last_print = (line, column, length, m.hl, m.b)

    def priat(self, m, color: int) -> None:
        assert self.mapped == TXT, "PRIAT без TXT в #C000"
        assert self.calls[-2] == FN_PRSRW, "PRIAT — только сразу за PRSRW"
        line, column, length, address, count = self.last_print
        assert (m.hl, m.b) == (address, count), "HL/B от PRSRW испорчены до PRIAT"
        colors = self.colors.setdefault(line, [0] * 80)
        colors[column - 1:column - 1 + length] = [color] * length

    def txtpr(self, m) -> None:
        """API 11: размеченный текст; #0D — на строку ниже, в ту же колонку.
        TXTPR сам подключает TXT и красит знаки цветом окна."""
        self.map_c000(TXT)
        assert not self.rx and self.memory[self.symbol("ProtoBurstLeft")] == 0, \
            "рисование при непрочитанных байтах ZiFi"
        assert m.hl < 0xC000, "текст TXTPR — не из окна #C000"
        line, column, address, count = m.d, m.e, m.hl, 0
        color = self.memory[m.ix + 6]
        while True:
            byte = self.memory[address]
            address += 1
            if byte == 0:
                break
            if byte == 0x0D:
                line, column = line + 1, m.e
                continue
            if byte in (0x0B, 0x0C):
                address += 1
                continue
            if byte < 10 or byte == 0x0E:
                continue
            self.screen.setdefault(line, bytearray(b" " * 80))[column - 1] = byte
            self.colors.setdefault(line, [0] * 80)[column - 1] = color
            self.prints.append((line, column, bytes([byte])))
            column += 1
            count += 1
        self.ticks += TXTPR_TICKS[0] + TXTPR_TICKS[1] * count
        self.flags(zero=True)

    def line(self, number: int) -> bytes:
        return bytes(self.screen.get(number, bytearray(b" " * 80)))[:70]

    def line_colors(self, number: int) -> list[int]:
        return self.colors.get(number, [0] * 80)[:70]

    # --- исполнение -----------------------------------------------------------------
    def run(self, pc: int, *, a: int = 0, ix: int = 0x7E00, hl: int = 0, bc: int = 0,
            de: int = 0, stack: tuple[int, ...] = ()) -> None:
        m = self.machine
        words = list(stack) + [SENTINEL]
        sp = STACK - 2 * len(words)
        for index, word in enumerate(words):
            self.memory[sp + 2 * index] = word & 0xFF
            self.memory[sp + 2 * index + 1] = word >> 8
        m.sp = sp
        m.pc = pc
        m.a = a
        m.ix = ix
        m.hl = hl
        m.bc = bc
        m.de = de
        for _ in range(400_000):
            m.ticks_to_stop = 5_000_000
            before = ticks_left(m)
            self.segment_start = before
            event = m.run()
            self.ticks += before - ticks_left(m)
            self.segment_start = ticks_left(m)
            if event & BREAKPOINT_HIT and m.pc == SENTINEL:
                return
            if event & BREAKPOINT_HIT and m.pc == API_BREAK:
                self.handle_api()
                continue
            if m.halted:
                self.interrupts += 1
                m.on_handle_active_int()
                continue
            if event & BREAKPOINT_HIT:
                raise AssertionError(f"неожиданный breakpoint #{m.pc:04X}")
        raise AssertionError("Z80 не завершил процедуру")

    def carry(self) -> bool:
        return bool(self.machine.f & 1)

    def zero(self) -> bool:
        return bool(self.machine.f & 0x40)

    # --- удобства сценариев ----------------------------------------------------------
    def open_window(self) -> None:
        self.run(self.symbol("Ui_Open"))
        self.prints.clear()

    def feed(self, *packets: bytes) -> None:
        for packet in packets:
            self.rx += packet
        self.run(self.symbol("Wcu_Serve"))

    def draw_all(self) -> list[int]:
        """Рисовать шагами до конца; вернуть число PRSRW на каждом шаге."""
        steps = []
        for _ in range(200):
            before = len(self.prints)
            self.run(self.symbol("Ui_DrawStep"))
            if self.zero():
                self.assertEmpty = before == len(self.prints)
                return steps
            steps.append(len(self.prints) - before)
        raise AssertionError("рисование не заканчивается")

    def key(self, name: str | None = None, *, kbscn: int | None = None) -> None:
        """Кадр с нажатием: окно сперва дорисовано, затем Ui_KeysFrame."""
        self.draw_all()
        if name is not None:
            self.pressed = {name}
        if kbscn is not None:
            self.kbscn = kbscn
        self.run(self.symbol("Ui_KeysFrame"))

    def set_request(self, payload: bytes) -> None:
        buffer = self.symbol("ProtoBuf")
        self.memory[buffer:buffer + len(payload)] = payload
        length = self.symbol("ProtoRxLen")
        self.memory[length] = len(payload) & 0xFF
        self.memory[length + 1] = len(payload) >> 8


def five_rows() -> list[bytes]:
    """Строки после проверки: исполняемые отличаются, текст новый, остальное —
    совпало или защищено."""
    return [
        entry(0, DIFF, F_REMOTE | F_LOCAL | F_CAN | F_AUTO, 7237, 7237, b"WC/FILEX.WMF"),
        entry(1, NEW, F_REMOTE | F_CAN, 0, 1835, b"Help.txt"),
        entry(2, SAME, F_REMOTE | F_LOCAL, 31761, 31761, b"boot.$C"),
        entry(3, KEPT_DIFF, F_REMOTE | F_LOCAL | F_KEPT, 900, 1631, b"WC/wc.ini"),
        entry(4, NEW, F_REMOTE | F_CAN | F_AUTO, 0, 158208, b"WC/TXTVIEW2.WMF"),
    ]


class WcUpdateBuildTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        require_build()

    def test_header(self) -> None:
        data = WMF.read_bytes()
        self.assertEqual(data[165:197].rstrip(b" "), b"ZiFi WC Update v1.0")
        self.assertEqual(data[197], 3, "плагин меню F10")
        self.assertEqual(data[34], 3, "страницы: код, окно записи, таблица списка")
        self.assertEqual(data[35], 0)
        self.assertEqual(data[38:48], bytes(10), "из файла грузится только код")
        self.assertLessEqual(len(data) - 512, 0x4000, "код — в одной странице")

    def test_start_payload_protects_wc_ini(self) -> None:
        h = Harness()
        address = h.symbol("WcuStartPayload")
        payload = bytes(h.memory[address:address + 80])
        parts = payload.split(b"\0")
        self.assertEqual(parts[:5], [b"andrewinsidelazarev/Wild-Commander-Improved",
                                     b"main", b"exe", b"WC/wc.ini", b""])


class WcUpdateEventsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        require_build()

    def test_entries_land_in_page_2(self) -> None:
        h = Harness()
        h.open_window()
        long_path = b"WC/MENU/" + b"x" * 40 + b".spg"     # 52 символа
        h.feed(entry(0, DIFF, F_REMOTE | F_LOCAL | F_CAN | F_AUTO, 7237, 7300,
                     b"WC/FILEX.WMF"),
               entry(1, NEW, F_REMOTE | F_CAN, 0, 1835,
                     "WC/Имя.txt".encode("utf-8")),
               entry(2, SAME, F_REMOTE | F_LOCAL, 158208, 158208, long_path))
        self.assertEqual(h.byte("WcuCount"), 3)
        row = h.table_entry(0)
        self.assertEqual(row[0], DIFF)
        self.assertEqual(row[1], F_REMOTE | F_LOCAL | F_CAN | F_AUTO)
        self.assertEqual(int.from_bytes(row[2:5], "little"), 7237)
        self.assertEqual(int.from_bytes(row[5:8], "little"), 7300)
        self.assertEqual(row[9:9 + row[8]], b"WC/FILEX.WMF")
        # UTF-8 -> CP866, как в панелях WC.
        row = h.table_entry(1)
        self.assertEqual(row[9:9 + row[8]], b"WC/\x88\xac\xef.txt")
        row = h.table_entry(2)
        self.assertEqual(row[9:9 + row[8]], long_path)
        self.assertEqual(int.from_bytes(row[2:5], "little"), 158208)

    def test_mark_survives_update_only_while_file_can_be_updated(self) -> None:
        h = Harness()
        h.open_window()
        h.feed(*five_rows())
        table = 0xC000
        # отметить 0 и 1 прямо в таблице (как Space)
        h.map_c000(2)
        h.memory[table + 1] |= F_MARK
        h.memory[table + ENTRY_SIZE + 1] |= F_MARK
        h.feed(entry(0, UPDATED, F_REMOTE | F_LOCAL, 7237, 7237, b"WC/FILEX.WMF"),
               entry(1, FAILED, F_REMOTE | F_CAN, 0, 1835, b"Help.txt"))
        self.assertEqual(h.table_entry(0)[1], F_REMOTE | F_LOCAL, "обновлён — отметка снята")
        self.assertEqual(h.table_entry(1)[1], F_REMOTE | F_CAN | F_MARK,
                         "неудача — отметка остаётся для повтора")

    def test_state_fills_status_and_bar(self) -> None:
        h = Harness()
        h.open_window()
        h.feed(state(PH_CHECK, b"SHA WC/FILEX.WMF", percent=47, cur=12, tot=30))
        self.assertEqual(h.byte("WcuPhase"), PH_CHECK)
        self.assertEqual(h.byte("WcuPercent"), 47)
        self.assertTrue(h.text("UiStatusBuf", 70).startswith(b"SHA WC/FILEX.WMF   "))
        h.draw_all()
        self.assertTrue(h.line(STATUS_LINE).startswith(b"SHA WC/FILEX.WMF"))
        bar = h.line(BAR_LINE)
        self.assertEqual(bar[1:2], b"[")
        cells = bar[2:52]
        self.assertEqual(cells.count(0xDB), 23, "47% — 23 клетки из 50")
        self.assertEqual(cells.count(0xB0), 27)
        self.assertEqual(bar[52:53], b"]")
        self.assertEqual(bar[54:].rstrip(), b"47%  12/30")

    def test_error_state_is_marked_and_red(self) -> None:
        h = Harness()
        h.open_window()
        h.feed(state(PH_ERROR, b"GitHub API limit, retry later"))
        self.assertTrue(h.text("UiStatusBuf", 70).startswith(
            b"ERROR: GitHub API limit, retry later"))
        h.draw_all()
        self.assertEqual(h.line_colors(STATUS_LINE)[0], UI_BAD)
        self.assertEqual(h.byte("WcuBusy"), 0)

    def test_ready_after_update_reminds_to_restart(self) -> None:
        h = Harness()
        h.open_window()
        h.memory[h.symbol("WcuApplied")] = 1
        h.memory[h.symbol("WcuBusy")] = 1
        h.feed(state(PH_READY, b"All files match GitHub 5dc0a43", percent=100))
        self.assertTrue(h.text("UiStatusBuf", 70).startswith(
            b"All files match GitHub 5dc0a43 - restart WC"))
        self.assertEqual(h.byte("WcuBusy"), 0, "итог снимает занятость")

    def test_bad_packets_are_ignored(self) -> None:
        h = Harness()
        h.open_window()
        h.feed(frame(EVT_WCU_ENTRY, bytes(8)),                    # короче заголовка
               entry(96, NEW, F_CAN, 0, 1, b"beyond.WMF"),        # за пределом таблицы
               frame(EVT_WCU_STATE, bytes(5)))
        self.assertEqual(h.byte("WcuCount"), 0)
        self.assertEqual(h.page(2)[:ENTRY_SIZE * 2], bytes(ENTRY_SIZE * 2))


class WcUpdateDrawTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        require_build()

    def test_rows_columns_and_colors(self) -> None:
        h = Harness()
        h.open_window()
        long_path = b"WC/MENU/" + b"y" * 30 + b" long name v1.2.spg"
        h.feed(*five_rows(),
               entry(5, LOCAL_ONLY, F_LOCAL, 13568, 0, b"WC/ZIFIFTP.WMF"),
               entry(6, READ_ERR, F_REMOTE | F_LOCAL | F_CAN | F_AUTO, 100, 100, long_path))
        h.memory[h.symbol("UiCursor")] = 1
        steps = h.draw_all()
        self.assertTrue(all(count <= 2 for count in steps[:-1]),
                        f"за шаг — одна строка (и слово состояния): {steps}")
        self.assertNotIn(FN_WAITUP, h.calls)
        rows = [h.line(LIST_LINE + index) for index in range(7)]
        self.assertEqual(rows[0][1:4], b"[ ]")
        self.assertEqual(rows[0][5:17], b"WC/FILEX.WMF")
        self.assertEqual(rows[0][42:49], b"   7237")
        self.assertEqual(rows[0][50:57], b"   7237")
        self.assertEqual(rows[0][58:66], b"DIFFERS ")
        self.assertEqual(rows[1][42:49], b"      -", "на SD файла нет")
        self.assertEqual(rows[1][58:61], b"NEW")
        self.assertEqual(rows[2][1:4], b"   ", "совпавший файл не отмечается")
        self.assertEqual(rows[2][58:62], b"same")
        self.assertEqual(rows[3][1:4], b"   ", "wc.ini не отмечается")
        self.assertEqual(rows[3][58:62], b"kept")
        self.assertEqual(rows[4][50:57], b" 158208")
        self.assertEqual(rows[5][50:57], b"      -", "на GitHub файла нет")
        self.assertEqual(rows[5][58:65], b"SD only")
        # Длинный путь: хвост с именем файла и '<' в начале поля.
        self.assertEqual(rows[6][5:6], b"<")
        self.assertTrue(rows[6][6:41].endswith(b"long name v1.2.spg"))
        self.assertEqual(rows[6][58:66], b"READ ERR")
        # Цвета: курсор — вся строка, у прочих — слово состояния.
        self.assertEqual(set(h.line_colors(LIST_LINE + 1)), {UI_CURSOR})
        self.assertEqual(h.line_colors(LIST_LINE)[58], UI_WARN)
        self.assertEqual(h.line_colors(LIST_LINE)[0], UI_COLOR)
        self.assertEqual(h.line_colors(LIST_LINE + 2)[58], UI_COLOR)
        self.assertEqual(h.line_colors(LIST_LINE + 6)[58], UI_BAD)
        self.assertEqual(h.line(LIST_LINE + 7).strip(), b"", "за концом списка — пусто")

    def test_sizes_over_seven_digits_are_shown_in_kib(self) -> None:
        h = Harness()
        h.open_window()
        h.feed(entry(0, DIFF, F_REMOTE | F_LOCAL | F_CAN, 12_345_678, 16_777_214,
                     b"WC/BIG.WMF"),
               entry(1, SAME, F_REMOTE | F_LOCAL, 9_999_999, 9_999_999, b"WC/MAX7.WMF"),
               entry(2, LOCAL_ONLY, F_LOCAL, 0xFFFFFF, 0, b"WC/HUGE.DAT"))
        h.draw_all()
        self.assertEqual(h.line(LIST_LINE)[42:49], b" 12056K")
        self.assertEqual(h.line(LIST_LINE)[50:57], b" 16383K")
        self.assertEqual(h.line(LIST_LINE + 1)[42:49], b"9999999")
        # #FFFFFF — ESP насыщает поле: 16 МиБ и больше.
        self.assertEqual(h.line(LIST_LINE + 2)[42:49], b"   >16M")

    def test_head_line(self) -> None:
        h = Harness()
        h.run(h.symbol("Ui_Open"))
        head = h.line(HEAD_LINE)
        self.assertEqual(head[:9], b" Upd File")
        self.assertEqual(head[42:49], b"     SD")
        self.assertEqual(head[50:57], b" GitHub")
        self.assertEqual(head[58:63], b"State")
        self.assertEqual(sorted(h.screen), [HEAD_LINE], "при открытии — только заголовок")

    def test_window_height_fits_every_text_mode(self) -> None:
        """Высота — как у окна FTP-сервера, 19 строк: влезает в 25, 30 и 36
        строк с тенью. Экран ниже — окно ужимается, но не меньше 12 строк."""
        for height, window_height in ((25, 19), (30, 19), (36, 19), (20, 18),
                                      (14, 12), (0, 12), (255, 19)):
            with self.subTest(height=height):
                h = Harness()
                h.memory[WC_HEI] = height
                h.run(h.symbol("Ui_Open"))
                window = h.symbol("WcuWindow")
                self.assertEqual(h.memory[window + 5], window_height)
                self.assertEqual(h.byte("UiRows"), window_height - 5)
                self.assertEqual(h.memory[window + 2:window + 4], bytes([0xFF, 0xFF]),
                                 "PRWOW центрирует окно")
                if 25 <= height <= 36:
                    self.assertLessEqual(window_height + 1, height, "с тенью")

    def test_scrolling_keeps_cursor_visible(self) -> None:
        h = Harness()
        h.open_window()
        h.feed(*[entry(index, SAME, F_REMOTE | F_LOCAL, index, index,
                       b"WC/F%02d.WMF" % index) for index in range(40)])
        h.key("end")
        self.assertEqual(h.byte("UiCursor"), 39)
        self.assertEqual(h.byte("UiTop"), 40 - LIST_ROWS, "курсор на нижней строке окна")
        h.draw_all()
        self.assertEqual(h.line(LIST_LINE + LIST_ROWS - 1)[5:14], b"WC/F39.WM")
        self.assertEqual(set(h.line_colors(LIST_LINE + LIST_ROWS - 1)), {UI_CURSOR})
        # Полоса прокрутки на правой рамке: ползунок внизу.
        scroll = [h.screen[LIST_LINE + row][70] for row in range(LIST_ROWS)]
        self.assertEqual(scroll[-1], 0xDB)
        self.assertEqual(scroll.count(0xDB), 1)
        h.key("home")
        self.assertEqual((h.byte("UiCursor"), h.byte("UiTop")), (0, 0))
        h.key("pgdn")
        self.assertEqual((h.byte("UiCursor"), h.byte("UiTop")), (LIST_ROWS, 1))
        h.key("up")
        self.assertEqual((h.byte("UiCursor"), h.byte("UiTop")), (LIST_ROWS - 1, 1))

    def test_scroll_waits_for_the_redraw_without_losing_presses(self) -> None:
        """Стрелка применяется, когда окно дорисовано: удержанная сдвигает
        список не быстрее перерисовки. Нажатие в недорисованное окно не
        теряется (WC хранит только текущее состояние клавиши) — плагин
        забирает его и применяет после, но не позже чем через 10 кадров.
        Esc — сразу."""
        h = Harness()
        h.open_window()
        h.feed(*[entry(index, SAME, F_REMOTE | F_LOCAL, index, index,
                       b"WC/F%02d.WMF" % index) for index in range(40)])
        h.draw_all()
        h.memory[h.symbol("UiCursor")] = LIST_ROWS - 1   # на нижней строке окна
        h.pressed = {"down"}
        h.run(h.symbol("Ui_KeysFrame"))                  # сдвиг: всё окно заново
        self.assertEqual((h.byte("UiCursor"), h.byte("UiTop")), (LIST_ROWS, 1))
        # Окно не дорисовано: удержанная стрелка (два срабатывания
        # автоповтора) — одно запомненное нажатие.
        for _ in range(2):
            h.pressed = {"down"}
            h.run(h.symbol("Ui_KeysFrame"))
            self.assertEqual(h.pressed, set(), "нажатие взято у WC")
        self.assertEqual(h.byte("UiCursor"), LIST_ROWS, "окно не дорисовано — ждём")
        h.run(h.symbol("Ui_KeysFrame"))
        self.assertEqual(h.byte("UiCursor"), LIST_ROWS)
        h.draw_all()
        h.run(h.symbol("Ui_KeysFrame"))
        self.assertEqual(h.byte("UiCursor"), LIST_ROWS + 1, "дорисовано — ровно шаг")
        h.draw_all()
        h.run(h.symbol("Ui_KeysFrame"))
        self.assertEqual(h.byte("UiCursor"), LIST_ROWS + 1, "повторы не копились")
        # Линия занята, рисовать нельзя: запомненная клавиша применяется на
        # десятом кадре, а Esc — сразу.
        h.run(h.symbol("Ui_DirtyAll"))
        h.pressed = {"up"}
        for frame_no in range(9):
            h.run(h.symbol("Ui_KeysFrame"))
            self.assertEqual(h.byte("UiCursor"), LIST_ROWS + 1, f"кадр {frame_no + 1}")
        h.run(h.symbol("Ui_KeysFrame"))
        self.assertEqual(h.byte("UiCursor"), LIST_ROWS)
        h.run(h.symbol("Ui_DirtyAll"))
        h.pressed = {"esc"}
        h.run(h.symbol("Ui_KeysFrame"))
        self.assertEqual(h.byte("WcuExit"), 1)

    def test_different_keys_keep_their_order(self) -> None:
        """Другая клавиша, пока прежняя ждёт перерисовку, сначала применяет
        прежнюю: вниз и вверх в недорисованное окно — курсор на месте."""
        h = Harness()
        h.open_window()
        h.feed(*[entry(index, SAME, F_REMOTE | F_LOCAL, index, index,
                       b"WC/F%02d.WMF" % index) for index in range(40)])
        h.draw_all()
        h.memory[h.symbol("UiCursor")] = 5
        h.run(h.symbol("Ui_DirtyAll"))
        h.pressed = {"down"}
        h.run(h.symbol("Ui_KeysFrame"))
        h.pressed = {"up"}
        h.run(h.symbol("Ui_KeysFrame"))
        h.draw_all()
        h.run(h.symbol("Ui_KeysFrame"))
        self.assertEqual(h.byte("UiCursor"), 5)


class WcUpdateTimingTest(unittest.TestCase):
    """Рисование не должно оставлять очередь ZiFi без чтения дольше, чем
    она наполняется: у ZiFi нет управления потоком."""

    @classmethod
    def setUpClass(cls) -> None:
        require_build()

    def test_counter_counts_z80_ticks(self) -> None:
        h = Harness()
        # LD B,10 ; DJNZ $ ; RET — 7 + 9*13 + 8 + 10 тактов
        h.machine.set_memory_block(0x7000, bytes([0x06, 10, 0x10, 0xFE, 0xC9]))
        h.run(0x7000)
        self.assertEqual(h.ticks, 7 + 9 * 13 + 8 + 10)

    def test_every_draw_step_fits_the_budget(self) -> None:
        h = Harness()
        h.memory[WC_HEI] = 36
        h.open_window()
        name = b"WC/MENU/" + "Ж".encode("utf-8") * 23      # 54 байта, хвост
        packets = [entry(index, READ_ERR, F_REMOTE | F_LOCAL | F_CAN | F_AUTO,
                         9_999_999 - index, 8_888_888 + index, name)
                   for index in range(60)]
        packets.append(state(PH_CHECK, b"SHA " + b"x" * 57, 99, 60, 60))
        h.feed(*packets)
        h.memory[h.symbol("UiTop")] = 30
        h.memory[h.symbol("UiCursor")] = 31
        h.run(h.symbol("Ui_DirtyAll"))
        worst = 0
        for _ in range(100):
            h.ticks = 0
            before = len(h.prints)
            h.run(h.symbol("Ui_DrawStep"))
            if h.zero():
                break
            worst = max(worst, h.ticks)
            self.assertLess(h.ticks, STEP_BUDGET,
                            f"шаг #{len(h.prints) - before} печатей: {h.ticks} тактов")
        self.assertGreater(worst, 5_000, "шаги действительно измерены")

    def test_entry_handler_is_short(self) -> None:
        """Разбор строки уже принятого пакета: сам приём байтов очередь и
        опустошает, а вот обработчик — время, когда она не читается."""
        h = Harness()
        h.open_window()
        payload = (bytes([0, DIFF, F_REMOTE | F_LOCAL | F_CAN | F_AUTO]) +
                   (1).to_bytes(3, "little") + (2).to_bytes(3, "little") +
                   "Ж".encode("utf-8") * 27)
        h.set_request(payload)
        h.ticks = 0
        h.run(h.symbol("Wcu_Entry"))
        self.assertLess(h.ticks, STEP_BUDGET // 2)
        self.assertEqual(h.byte("WcuCount"), 1)


class WcUpdateFifoTest(unittest.TestCase):
    """Главный цикл при настоящей линии: события идут сплошным потоком,
    а окно тем временем перерисовывает список. Очередь ZiFi не должна
    переполниться (у ZiFi нет управления потоком)."""

    @classmethod
    def setUpClass(cls) -> None:
        require_build()

    def test_event_burst_while_drawing_loses_nothing(self) -> None:
        h = Harness()
        h.memory[WC_HEI] = 36
        h.open_window()
        h.memory[h.symbol("WcuStarted")] = 1
        h.memory[h.symbol("WcuSyncsLeft")] = 5
        name = b"WC/MENU/" + "Ж".encode("utf-8") * 23
        burst = bytearray()
        for index in range(90):
            burst += state(PH_CHECK, b"SHA " + name[:50], index, index, 90)
            burst += entry(index, DIFF, F_REMOTE | F_LOCAL | F_CAN | F_AUTO,
                           9_999_999 - index, 8_888_888 + index, name)
        burst += state(PH_READY, b"90 to update from GitHub 5dc0a43", 100, 90, 90)
        stage = {"step": 0}

        clock = {"frame": float(FRAME_TICKS)}

        def esp(h: Harness) -> None:
            while h.now() >= clock["frame"]:
                h.next_frame()
                clock["frame"] += FRAME_TICKS
            if stage["step"] == 0:
                stage["step"] = 1
                h.send(bytes(burst))
            elif (stage["step"] == 1 and not h.wire and not h.rx and
                  h.byte("WcuPhase") == PH_READY):
                stage["step"] = 2
                h.pressed.add("esc")
            elif stage["step"] == 2 and CMD_WCU_STOP in h.commands():
                stage["step"] = 3
                h.send(frame(RESP_ACK) + frame(RESP_WCU_STOP, b"\x01"))
        h.esp = esp
        h.run(h.symbol("PLUGIN.loop"), stack=(0x7E00,))
        self.assertEqual(h.lost, 0, f"пик очереди {h.fifo_peak} байт")
        self.assertEqual(h.byte("WcuCount"), 90)
        self.assertEqual(h.byte("WcuSyncWanted"), 0, "все строки дошли")
        self.assertLess(h.fifo_peak, ZIFI_FIFO // 2,
                        "запас очереди не меньше половины")
        self.assertGreater(len(h.prints), 20, "окно при этом рисовалось")


class WcUpdateKeysTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        require_build()

    def ready(self) -> Harness:
        h = Harness()
        h.open_window()
        h.feed(*five_rows(), state(PH_READY, b"3 to update from GitHub 5dc0a43", 100))
        return h

    def marks(self, h: Harness) -> list[int]:
        return [index for index in range(5) if h.table_entry(index)[1] & F_MARK]

    def answer(self, command: int, response: int, *extra: bytes):
        """ESP: на команду — ACK и ответ [1], затем дополнительные пакеты."""
        def esp(h: Harness) -> None:
            if command in h.commands() and not getattr(h, "answered", False):
                h.answered = True
                h.rx += frame(RESP_ACK) + frame(response, b"\x01")
                for packet in extra:
                    h.rx += packet
        return esp

    def test_space_marks_only_updatable_and_moves_down(self) -> None:
        h = self.ready()
        for _ in range(4):
            h.key("space")
        self.assertEqual(self.marks(h), [0, 1], "совпавший и wc.ini не отмечаются")
        self.assertEqual(h.byte("UiCursor"), 4)
        h.memory[h.symbol("UiCursor")] = 0
        h.key("space")
        self.assertEqual(self.marks(h), [1], "повторный пробел снимает отметку")

    def test_a_selects_executables_and_keeps_manual_marks(self) -> None:
        h = self.ready()
        h.memory[h.symbol("UiCursor")] = 1
        h.key("space")                             # вручную — текст
        h.key(kbscn=ord("A"))
        self.assertEqual(self.marks(h), [0, 1, 4])
        h.key(kbscn=ord("a"))                      # автоповтор: ничего не меняет
        self.assertEqual(self.marks(h), [0, 1, 4])

    def test_space_then_enter_in_one_frame_keeps_order(self) -> None:
        """Пробел ждёт перерисовку, но Enter в том же кадре сначала его
        применяет: снятая пробелом отметка в APPLY не уходит."""
        h = self.ready()
        h.key(kbscn=ord("a"))                      # отмечены 0 и 4
        h.run(h.symbol("Ui_DirtyAll"))             # окно не дорисовано
        h.esp = self.answer(CMD_WCU_APPLY, RESP_WCU_APPLY)
        h.memory[h.symbol("UiCursor")] = 0
        h.pressed = {"space", "enter"}
        h.run(h.symbol("Ui_KeysFrame"))
        self.assertEqual([item for item in h.frames() if item[0] == CMD_WCU_APPLY],
                         [(CMD_WCU_APPLY, bytes([4]))])

    def test_enter_sends_marked_indices(self) -> None:
        h = self.ready()
        h.key(kbscn=ord("a"))
        h.esp = self.answer(CMD_WCU_APPLY, RESP_WCU_APPLY)
        h.key("enter")
        frames = [frame_ for frame_ in h.frames() if frame_[0] == CMD_WCU_APPLY]
        self.assertEqual(frames, [(CMD_WCU_APPLY, bytes([0, 4]))])
        self.assertNotIn(FN_WAITUP, h.calls,
                         "никаких блокирующих ожиданий: очередь ZiFi читается")
        self.assertEqual(h.byte("WcuBusy"), 1)
        self.assertEqual(h.byte("WcuApplied"), 1)
        self.assertEqual(h.byte("WcuRefreshAfter"), 1, "после итога — повтор списка")
        # Пока идёт обновление, отметки и Enter не принимаются.
        h.key("space")
        self.assertEqual(self.marks(h), [0, 4])
        before = len(h.tx)
        h.key("enter")
        self.assertEqual(len(h.tx), before)

    def test_enter_without_marks_takes_cursor_row(self) -> None:
        h = self.ready()
        h.memory[h.symbol("UiCursor")] = 1
        h.esp = self.answer(CMD_WCU_APPLY, RESP_WCU_APPLY)
        h.pressed = {"enter"}
        h.run(h.symbol("Ui_Keys"))
        self.assertIn((CMD_WCU_APPLY, bytes([1])), h.frames())

    def test_enter_on_row_that_cannot_update_sends_nothing(self) -> None:
        h = self.ready()
        h.memory[h.symbol("UiCursor")] = 2           # совпавший boot.$C
        h.pressed = {"enter"}
        h.run(h.symbol("Ui_Keys"))
        self.assertEqual(h.tx, bytearray())
        self.assertEqual(h.byte("WcuBusy"), 0)

    def test_keys_ignored_before_check_ends(self) -> None:
        h = Harness()
        h.open_window()
        h.feed(*five_rows(), state(PH_CHECK, b"SHA WC/FILEX.WMF", 50, 3, 5))
        h.key("space")
        h.key(kbscn=ord("a"))
        h.key("enter")
        self.assertEqual(self.marks(h), [])
        self.assertEqual(h.tx, bytearray())

    def test_enter_waits_while_the_list_is_refreshed(self) -> None:
        h = self.ready()
        h.memory[h.symbol("UiCursor")] = 0
        h.memory[h.symbol("WcuSyncPending")] = 1
        h.pressed = {"enter"}
        h.run(h.symbol("Ui_Keys"))
        self.assertEqual(h.tx, bytearray())
        self.assertEqual(h.byte("WcuBusy"), 0)

    def test_result_inside_apply_wait_still_refreshes(self) -> None:
        """Итог APPLY пришёл раньше ответа A6: занятость снята, а повтор
        списка всё равно попрошен — флаг ставится до отправки."""
        h = self.ready()
        h.memory[h.symbol("UiCursor")] = 0

        def esp(h: Harness) -> None:
            if CMD_WCU_APPLY in h.commands() and not getattr(h, "answered", False):
                h.answered = True
                h.rx += (frame(RESP_ACK) +
                         state(PH_APPLY, b"Update WC/FILEX.WMF", 10, 1, 1) +
                         state(PH_READY, b"All files match GitHub 5dc0a43", 100, 0, 5) +
                         frame(RESP_WCU_APPLY, b"\x01"))
        h.esp = esp
        h.pressed = {"enter"}
        h.run(h.symbol("Ui_Keys"))
        self.assertEqual(h.byte("WcuBusy"), 0)
        self.assertEqual(h.byte("WcuRefreshWanted"), 1)
        self.assertEqual(h.byte("WcuRefreshAfter"), 0)

    def test_lost_apply_answer_times_out_fast_and_still_refreshes(self) -> None:
        """ACK есть, A6 потерян: ESP отвечает на APPLY сразу, поэтому ждём
        5 с, а не 40. Если APPLY всё же идёт, его события сами просят
        повтор списка после итога."""
        h = self.ready()
        h.memory[h.symbol("UiCursor")] = 0

        def esp(h: Harness) -> None:
            if h.rx_polls % 40 == 0:
                h.next_frame()
            if CMD_WCU_APPLY in h.commands() and not getattr(h, "acked", False):
                h.acked = True
                h.ack_frame = h.word(WC_FRAMES)
                h.rx += frame(RESP_ACK)
        h.esp = esp
        h.pressed = {"enter"}
        h.run(h.symbol("Ui_Keys"))
        waited = h.word(WC_FRAMES) - h.ack_frame
        self.assertGreaterEqual(waited, 250)
        self.assertLess(waited, 300)
        self.assertEqual(h.byte("WcuBusy"), 0)
        self.assertTrue(h.text("UiStatusBuf", 70).startswith(b"ERROR: ESP did not accept"))
        h.esp = None
        h.feed(state(PH_APPLY, b"Update WC/FILEX.WMF", 10, 1, 1),
               state(PH_READY, b"All files match GitHub 5dc0a43", 100, 0, 5))
        self.assertEqual(h.byte("WcuRefreshWanted"), 1)

    def test_esc_during_apply_wait_exits_at_once(self) -> None:
        h = self.ready()
        h.memory[h.symbol("UiCursor")] = 0

        def esp(h: Harness) -> None:
            if h.rx_polls % 40 == 0:
                h.next_frame()
            if CMD_WCU_APPLY in h.commands() and not getattr(h, "acked", False):
                h.acked = True
                h.ack_frame = h.word(WC_FRAMES)
                h.rx += frame(RESP_ACK)
            if (getattr(h, "acked", False) and not getattr(h, "escaped", False) and
                    h.word(WC_FRAMES) - h.ack_frame >= 20):
                h.escaped = True
                h.pressed.add("esc")
        h.esp = esp
        h.pressed = {"enter"}
        h.run(h.symbol("Ui_Keys"))
        self.assertEqual(h.byte("WcuExit"), 1)
        self.assertLess(h.word(WC_FRAMES) - h.ack_frame, 25)
        self.assertEqual(h.byte("WcuBusy"), 0)
        self.assertFalse(h.text("UiStatusBuf", 70).startswith(b"ERROR"))

    def test_refused_apply_frees_the_list(self) -> None:
        h = self.ready()
        h.memory[h.symbol("UiCursor")] = 0

        def esp(h: Harness) -> None:
            if CMD_WCU_APPLY in h.commands() and not getattr(h, "answered", False):
                h.answered = True
                h.rx += frame(RESP_ACK) + frame(RESP_WCU_APPLY, b"\x00")
        h.esp = esp
        h.pressed = {"enter"}
        h.run(h.symbol("Ui_Keys"))
        self.assertEqual(h.byte("WcuBusy"), 0)
        self.assertTrue(h.text("UiStatusBuf", 70).startswith(b"ERROR: ESP did not accept"))

    def test_esc_requests_exit(self) -> None:
        h = self.ready()
        h.pressed = {"esc"}
        h.run(h.symbol("Ui_Keys"))
        self.assertEqual(h.byte("WcuExit"), 1)


class WcUpdateRequestTest(unittest.TestCase):
    """Команда без ACK уходит снова; пока ждём — файловые запросы обслуживаются."""

    @classmethod
    def setUpClass(cls) -> None:
        require_build()

    def test_lost_command_is_sent_again_and_vfs_is_served(self) -> None:
        h = Harness()
        h.open_window()
        sent_frames: list[int] = []

        def esp(h: Harness) -> None:
            if h.rx_polls % 40 == 0:
                h.next_frame()
            starts = h.commands().count(CMD_WCU_STOP)
            if starts > len(sent_frames):
                sent_frames.append(h.word(WC_FRAMES))
                if starts == 1:
                    # ESP занята файловым обменом: команда потеряна, а
                    # следующий файловый запрос уже в пути.
                    h.rx += frame(VFS_MKDIR, b"/NEWDIR\0")
                elif starts == 2:
                    h.rx += frame(RESP_ACK) + frame(RESP_WCU_STOP, b"\x01")
        h.esp = esp
        h.run(h.symbol("Wcu_Stop"))
        self.assertFalse(h.carry())
        self.assertEqual(h.machine.a, 1)
        commands = h.commands()
        self.assertEqual(commands.count(CMD_WCU_STOP), 2, commands)
        self.assertIn(VFS_MKDIR, commands, "файловый запрос обслужен во время ожидания")
        self.assertEqual(h.byte("VfsChanged"), 1)
        self.assertGreaterEqual(sent_frames[1] - sent_frames[0], 25,
                                "повтор — не раньше чем через полсекунды без ACK")

    def test_gives_up_without_any_ack(self) -> None:
        h = Harness()
        h.open_window()

        def esp(h: Harness) -> None:
            if h.rx_polls % 40 == 0:
                h.next_frame()
        h.esp = esp
        h.run(h.symbol("Wcu_Stop"))
        self.assertTrue(h.carry())
        self.assertEqual(h.commands().count(CMD_WCU_STOP), 6)
        self.assertEqual(h.byte("WcuReqAcked"), 0)

    def test_ack_then_answer_later(self) -> None:
        h = Harness()
        h.open_window()

        def esp(h: Harness) -> None:
            if h.rx_polls % 40 == 0:
                h.next_frame()
            if CMD_WCU_START in h.commands() and not getattr(h, "acked", False):
                h.acked = True
                h.ack_frame = h.word(WC_FRAMES)
                h.rx += frame(RESP_ACK)
            if getattr(h, "acked", False) and not getattr(h, "answered", False) \
                    and h.word(WC_FRAMES) - h.ack_frame >= 100:
                h.answered = True
                h.rx += state(1, b"GitHub: owner/repo") + frame(RESP_WCU_START, b"\x01")
        h.esp = esp
        h.run(h.symbol("Wcu_Start"))
        self.assertFalse(h.carry())
        self.assertEqual(h.commands().count(CMD_WCU_START), 1, "после ACK не повторять")
        self.assertEqual(h.byte("WcuReqAcked"), 1)
        self.assertTrue(h.text("UiStatusBuf", 70).startswith(b"GitHub: owner/repo"),
                        "событие во время ожидания разобрано")

    def test_esc_while_waiting_for_start(self) -> None:
        """Esc во время ожидания ответа START: выход сразу и без ошибки на
        экране; ESP команду узнала (ACK) — при выходе STOP."""
        h = Harness()
        h.open_window()

        def esp(h: Harness) -> None:
            if h.rx_polls % 40 == 0:
                h.next_frame()
            commands = h.commands()
            if CMD_WCU_START in commands and not getattr(h, "acked", False):
                h.acked = True
                h.ack_frame = h.word(WC_FRAMES)
                h.rx += frame(RESP_ACK)
            if (getattr(h, "acked", False) and not getattr(h, "escaped", False) and
                    h.word(WC_FRAMES) - h.ack_frame >= 30):
                h.escaped = True
                h.pressed.add("esc")
            if CMD_WCU_STOP in commands and not getattr(h, "stopped", False):
                h.stopped = True
                h.rx += frame(RESP_ACK) + frame(RESP_WCU_STOP, b"\x01")
        h.esp = esp
        h.run(h.symbol("PLUGIN.start"), stack=(0x7E00,))
        self.assertIn(CMD_WCU_STOP, h.commands())
        self.assertLess(h.word(WC_FRAMES) - h.ack_frame, 60, "Esc — сразу, не через 40 с")
        self.assertFalse(any(text.startswith(b"ERROR") for line, _, text in h.prints
                             if line == STATUS_LINE))

    def test_old_firmware_error_is_kept(self) -> None:
        h = Harness()
        h.open_window()

        def esp(h: Harness) -> None:
            if h.rx_polls % 40 == 0:
                h.next_frame()
            if h.commands().count(CMD_WCU_START) > getattr(h, "seen", 0):
                h.seen = h.commands().count(CMD_WCU_START)
                h.rx += frame(0xEE, b"unsupported:25")
        h.esp = esp
        h.run(h.symbol("Wcu_Start"))
        self.assertTrue(h.carry())
        self.assertEqual(h.byte("ProtoErr"), 1)
        self.assertEqual(h.text("ProtoErrText", 15), b"unsupported:25\0")


class WcUpdateRenameTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        require_build()

    def test_rename_in_directory(self) -> None:
        h = Harness()
        h.set_request(b"\x00/WC/WCUPD.TMP\x00FILEX.WMF\x00")
        h.run(h.symbol("Vfs_Rename"))
        self.assertEqual(h.renames, [(b"\x00WCUPD.TMP", b"FILEX.WMF")])
        self.assertEqual(h.frames(), [(VFS_RENAME, b"\x00")])
        self.assertEqual(h.byte("VfsChanged"), 1)

    def test_rename_in_root_with_utf8_name(self) -> None:
        h = Harness()
        h.set_request(b"\x00/WCUPD.TMP\x00" + "Имя.txt".encode("utf-8") + b"\x00")
        h.run(h.symbol("Vfs_Rename"))
        self.assertEqual(h.renames, [(b"\x00WCUPD.TMP", b"\x88\xac\xef.txt")])

    def test_wc_rename_code_goes_to_esp(self) -> None:
        """Код отказа RENAME от WC — в ответе: #FF (не удался и откат)
        ESP должна отличать от отказа без изменений."""
        for code, changed in ((0xFF, 1), (8, 0), (0, 0)):
            with self.subTest(code=code):
                h = Harness()
                h.rename_code = code
                h.set_request(b"\x00/WC/WCUPD.TMP\x00FILEX.WMF\x00")
                h.run(h.symbol("Vfs_Rename"))
                self.assertEqual(h.frames(), [(VFS_RENAME, bytes([code or 1]))])
                self.assertEqual(h.byte("VfsChanged"), changed)

    def test_bad_rename_requests_are_refused(self) -> None:
        cases = {
            "short": b"\x00/A\x00",
            "tail after name": b"\x00/WCUPD.TMP\x00A.WMF\x00junk",
            "no new name end": b"\x00/WCUPD.TMP\x00A.WMF",
            "missing directory": b"\x00/NODIR/WCUPD.TMP\x00A.WMF\x00",
            "root": b"\x00/\x00A.WMF\x00",
        }
        for name, payload in cases.items():
            with self.subTest(name):
                h = Harness()
                h.set_request(payload)
                h.run(h.symbol("Vfs_Rename"))
                self.assertEqual(h.renames, [])
                self.assertEqual(h.frames(), [(VFS_RENAME, b"\x01")])
                self.assertEqual(h.byte("VfsChanged"), 0)


class WcUpdateOpenTest(unittest.TestCase):
    """OPEN на запись в обновляторе берёт только свободное имя."""

    @classmethod
    def setUpClass(cls) -> None:
        require_build()

    def test_existing_file_is_never_deleted(self) -> None:
        """Остаток WCUPD.TMP может делить цепочку с файлом: OPEN на запись
        его не удаляет — занятое имя отвергает MKFILE, ответ — отказ."""
        h = Harness()
        h.files = {b"WCUPD.TMP"}
        h.set_request(b"\x01/WC/WCUPD.TMP\x00")
        h.run(h.symbol("Vfs_Open"))
        self.assertEqual(h.frames(), [(VFS_OPEN, b"\x01")])
        self.assertNotIn(FN_DELETE, h.calls)
        self.assertEqual(h.mkfiles, [b"WCUPD.TMP"])

    def test_free_name_is_created_and_opened(self) -> None:
        h = Harness()
        h.set_request(b"\x01/WC/WCUPD.TMP\x00")
        h.run(h.symbol("Vfs_Open"))
        self.assertEqual(h.frames()[0][0], VFS_OPEN)
        self.assertEqual(h.frames()[0][1][0], 0, "открыт")
        self.assertIn(b"WCUPD.TMP", h.files)
        self.assertNotIn(FN_DELETE, h.calls)
        self.assertNotIn(FN_FENTRY, h.calls[:h.calls.index(FN_MKFILE)][-1:],
                         "перед MKFILE имя не ищется для удаления")
        self.assertEqual(h.byte("VfsChanged"), 1)


class WcUpdateMoveTest(unittest.TestCase):
    """MOVE_RENAME с REPLACE: имя файла не пропадает ни на миг."""

    @classmethod
    def setUpClass(cls) -> None:
        require_build()

    def test_replace_in_subdirectory(self) -> None:
        h = Harness()
        h.set_request(b"\x01\x00/WC/MENU/WCUPD.TMP\x00/WC/MENU/Hrust v1.3.spg\x00")
        h.run(h.symbol("Vfs_MoveRename"))
        self.assertEqual(h.moves, [{
            "flags": 1,
            "source": b"\x00WCUPD.TMP\x00",
            "dest": b"\x00Hrust v1.3.spg\x00",
            "source_dir": 0x789,
            "dest_dir": 0x789,
        }])
        self.assertEqual(h.frames(), [(VFS_MOVE_RENAME, b"\x00")])
        self.assertEqual(h.byte("VfsChanged"), 1)

    def test_replace_in_root_and_cluster_mask(self) -> None:
        h = Harness()
        h.set_request(b"\x01\x00/WCUPD.TMP\x00/boot.$C\x00")
        h.run(h.symbol("Vfs_MoveRename"))
        self.assertEqual((h.moves[0]["source_dir"], h.moves[0]["dest_dir"]), (0, 0))
        h = Harness()
        h.set_request(b"\x01\x00/WC/WCUPD.TMP\x00/WC/FILEX.WMF\x00")
        h.run(h.symbol("Vfs_MoveRename"))
        self.assertEqual(h.moves[0]["dest_dir"], 0x00123456,
                         "старшие 4 бита +20 в номер кластера не входят")

    def test_filex_status_goes_back_as_is(self) -> None:
        for status in (0x25, 0x1C, 0x21, 0x24):
            with self.subTest(status=hex(status)):
                h = Harness()
                h.filex_status = status
                h.set_request(b"\x01\x00/WC/WCUPD.TMP\x00/WC/A.WMF\x00")
                h.run(h.symbol("Vfs_MoveRename"))
                self.assertEqual(h.frames(), [(VFS_MOVE_RENAME, bytes([status]))])
                self.assertEqual(h.byte("VfsChanged"), 1, "перенос мог лечь")

    def test_without_filex_move_nothing_is_touched(self) -> None:
        h = Harness()
        h.filex_caps = 0x2F                     # всё, кроме MOVE_RENAME
        h.set_request(b"\x01\x00/WC/WCUPD.TMP\x00/WC/A.WMF\x00")
        h.run(h.symbol("Vfs_MoveRename"))
        self.assertEqual(h.moves, [])
        self.assertEqual(h.frames(), [(VFS_MOVE_RENAME, b"\xfe")])
        self.assertEqual(h.byte("VfsChanged"), 0)

    def test_bad_move_requests_are_refused(self) -> None:
        cases = {
            "short": b"\x01\x00/A\x00",
            "tail": b"\x01\x00/WC/A\x00/WC/B\x00x",
            "no end": b"\x01\x00/WC/A\x00/WC/B",
            "missing dir": b"\x01\x00/NO/A\x00/NO/B\x00",
            "root source": b"\x01\x00/\x00/WC/B\x00",
        }
        for name, payload in cases.items():
            with self.subTest(name):
                h = Harness()
                h.set_request(payload)
                h.run(h.symbol("Vfs_MoveRename"))
                self.assertEqual(h.moves, [])
                self.assertEqual(h.frames(), [(VFS_MOVE_RENAME, b"\x01")])

    def test_move_is_routed_from_the_main_loop(self) -> None:
        h = Harness()
        h.open_window()
        h.feed(frame(VFS_MOVE_RENAME, b"\x01\x00/WC/WCUPD.TMP\x00/WC/A.WMF\x00"))
        self.assertEqual(len(h.moves), 1)


class WcUpdateSyncTest(unittest.TestCase):
    """Потерянное событие: плагин просит ESP выдать список заново. Повтор —
    метка (этап 6, всего — число строк), строки 0..N-1 и последнее
    состояние; полон он, только если строки пришли подряд. SYNC уходит без
    ожидания ответа."""

    @classmethod
    def setUpClass(cls) -> None:
        require_build()

    def answering(self) -> Harness:
        h = Harness()
        h.open_window()
        h.memory[h.symbol("WcuSyncsLeft")] = 5
        return h

    def at(self, h: Harness, frames: int) -> None:
        """Кадровый счётчик — через frames кадров после последнего пакета."""
        start = h.word(h.symbol("WcuLastPacket"))
        h.memory[WC_FRAMES:WC_FRAMES + 2] = ((start + frames) & 0xFFFF).to_bytes(2, "little")

    def test_ready_with_missing_rows_asks_sync(self) -> None:
        h = self.answering()
        rows = five_rows()
        h.feed(*rows[:3], rows[4], state(PH_READY, b"2 to update", 100, 2, 5))
        self.assertEqual(h.byte("WcuSyncWanted"), 1, "строки 3 нет — дыра")
        polls = h.rx_polls
        h.run(h.symbol("Wcu_Watch"))
        self.assertEqual(h.commands(), [CMD_WCU_SYNC])
        self.assertEqual(h.rx_polls, polls, "ответа A8 не ждём")
        self.assertEqual(h.byte("WcuSyncWanted"), 0)
        self.assertEqual(h.byte("WcuSyncsLeft"), 4)
        self.assertEqual(h.byte("WcuSyncPending"), 1)
        h.feed(frame(RESP_ACK), frame(RESP_WCU_SYNC, b"\x01"),
               generation(rows, state(PH_READY, b"2 to update", 100, 2, 5)))
        self.assertEqual(h.byte("WcuSyncPending"), 0, "полный повтор")
        self.assertEqual(h.byte("WcuSyncWanted"), 0)
        self.assertEqual(h.table_entry(3)[0], KEPT_DIFF)

    def test_ready_with_fewer_rows_than_total_asks_sync(self) -> None:
        h = self.answering()
        h.feed(*five_rows()[:4], state(PH_READY, b"2 to update", 100, 2, 5))
        self.assertEqual(h.byte("WcuSyncWanted"), 1)

    def test_complete_list_needs_no_sync(self) -> None:
        h = self.answering()
        h.feed(*five_rows(), state(PH_READY, b"2 to update", 100, 2, 5))
        self.assertEqual(h.byte("WcuSyncWanted"), 0)
        for _ in range(3):
            h.run(h.symbol("Wcu_Watch"))
        self.assertEqual(h.tx, bytearray())

    def test_marker_keeps_phase_and_status(self) -> None:
        h = self.answering()
        h.feed(*five_rows(), state(PH_READY, b"2 to update", 100, 2, 5))
        h.feed(state(PH_SYNC, b"", 0, 0, 5))
        self.assertEqual(h.byte("WcuPhase"), PH_READY)
        self.assertTrue(h.text("UiStatusBuf", 70).startswith(b"2 to update"))
        self.assertEqual(h.byte("WcuSyncOn"), 1)

    def test_incomplete_repeat_is_asked_again(self) -> None:
        h = self.answering()
        rows = five_rows()
        h.feed(*rows[:4], state(PH_READY, b"2 to update", 100, 2, 5))
        h.run(h.symbol("Wcu_Watch"))                      # нет строки 4
        self.assertEqual(h.commands().count(CMD_WCU_SYNC), 1)
        # В повторе потеряна строка 2: итог ожидание не снимает.
        h.feed(state(PH_SYNC, b"", 0, 0, 5), rows[0], rows[1], rows[3], rows[4],
               state(PH_READY, b"2 to update", 100, 2, 5))
        self.assertEqual(h.byte("WcuSyncPending"), 1)
        self.assertEqual(h.byte("WcuSyncWanted"), 1)
        h.run(h.symbol("Wcu_Watch"))
        self.assertEqual(h.commands().count(CMD_WCU_SYNC), 2)
        self.assertEqual(h.byte("WcuSyncsLeft"), 3)
        h.feed(generation(rows, state(PH_READY, b"2 to update", 100, 2, 5)))
        self.assertEqual(h.byte("WcuSyncPending"), 0)

    def test_updated_row_lost_twice_is_caught(self) -> None:
        """Codex Н5: обновлённая строка потеряна, а затем та же строка — и в
        повторе. Число строк верное, дыр в таблице нет, но повтор неполон:
        ожидание не снимается, пока строка не придёт."""
        h = self.answering()
        rows = five_rows()
        h.feed(*rows, state(PH_READY, b"2 to update", 100, 2, 5))
        h.memory[h.symbol("WcuRefreshAfter")] = 1
        h.feed(state(PH_APPLY, b"Update WC/FILEX.WMF", 50, 1, 1),
               state(PH_READY, b"All files match GitHub 5dc0a43", 100, 0, 5))
        h.run(h.symbol("Wcu_Watch"))
        self.assertEqual(h.commands(), [CMD_WCU_SYNC])
        updated = entry(0, UPDATED, F_REMOTE | F_LOCAL, 7237, 7237, b"WC/FILEX.WMF")
        h.feed(state(PH_SYNC, b"", 0, 0, 5), *rows[1:],
               state(PH_READY, b"All files match GitHub 5dc0a43", 100, 0, 5))
        self.assertEqual(h.byte("WcuSyncPending"), 1)
        self.assertEqual(h.byte("WcuSyncWanted"), 1)
        self.assertEqual(h.table_entry(0)[0], DIFF, "строка ещё старая")
        h.run(h.symbol("Wcu_Watch"))
        h.feed(generation([updated] + rows[1:],
                          state(PH_READY, b"All files match GitHub 5dc0a43", 100, 0, 5)))
        self.assertEqual(h.byte("WcuSyncPending"), 0)
        self.assertEqual(h.table_entry(0)[0], UPDATED)

    def test_repeat_without_marker_does_not_end_waiting(self) -> None:
        h = self.answering()
        rows = five_rows()
        h.feed(*rows[:4], state(PH_READY, b"2 to update", 100, 2, 5))
        h.run(h.symbol("Wcu_Watch"))
        h.feed(*rows, state(PH_READY, b"2 to update", 100, 2, 5))   # метка потеряна
        self.assertEqual(h.byte("WcuSyncPending"), 1)
        self.assertEqual(h.byte("WcuSyncWanted"), 0, "повтор — по тишине, не сразу")

    def test_long_silence_while_busy_asks_sync_within_budget(self) -> None:
        h = self.answering()
        h.feed(state(PH_CHECK, b"SHA WC/X", 10, 1, 5))
        start = h.word(h.symbol("WcuLastPacket"))
        for frame_no in range(1, 3000):
            if frame_no % 500 == 0:
                h.memory[WC_FRAMES:WC_FRAMES + 2] = ((start + frame_no) & 0xFFFF).to_bytes(2, "little")
                h.run(h.symbol("Wcu_Watch"))
        self.assertEqual(h.tx, bytearray(), "59 с тишины — ещё не потеря")
        sent = 0
        for step in range(8):
            now = start + 3000 * (step + 1) + step
            h.memory[WC_FRAMES:WC_FRAMES + 2] = (now & 0xFFFF).to_bytes(2, "little")
            h.run(h.symbol("Wcu_Watch"))
            sent = h.commands().count(CMD_WCU_SYNC)
        self.assertEqual(sent, 5, "не больше пяти SYNC за сеанс")
        self.assertEqual(h.byte("WcuSyncPending"), 0)
        self.assertTrue(h.text("UiStatusBuf", 70).startswith(b"ERROR: list not refreshed"))

    def test_silence_while_apply_is_busy_asks_sync(self) -> None:
        """Потеряны все события APPLY и его итог: этап по-прежнему READY, но
        плагин занят — через 60 с тишины SYNC, и итог повтора снимает
        занятость."""
        h = self.answering()
        rows = five_rows()
        h.feed(*rows, state(PH_READY, b"2 to update", 100, 2, 5))
        h.memory[h.symbol("WcuBusy")] = 1
        self.at(h, 2999)
        h.run(h.symbol("Wcu_Watch"))
        self.assertEqual(h.tx, bytearray())
        self.at(h, 3001)
        h.run(h.symbol("Wcu_Watch"))
        self.assertEqual(h.commands(), [CMD_WCU_SYNC])
        h.feed(generation(rows, state(PH_READY, b"1 to update", 100, 1, 5)))
        self.assertEqual(h.byte("WcuBusy"), 0)
        self.assertEqual(h.byte("WcuSyncPending"), 0)

    def test_list_is_refreshed_after_every_apply(self) -> None:
        """После APPLY строки меняются на месте: потерю обновлённой ENTRY не
        видно по дырам, поэтому итог обновления всегда вызывает SYNC — и
        вне лимита на потери."""
        h = self.answering()
        h.memory[h.symbol("WcuSyncsLeft")] = 0
        rows = five_rows()
        h.feed(*rows, state(PH_READY, b"2 to update", 100, 2, 5))
        h.memory[h.symbol("WcuRefreshAfter")] = 1          # APPLY был принят
        h.feed(state(PH_APPLY, b"Update WC/FILEX.WMF", 50, 1, 1),
               state(PH_READY, b"All files match GitHub 5dc0a43", 100, 0, 5))
        self.assertEqual(h.byte("WcuRefreshWanted"), 1)
        h.run(h.symbol("Wcu_Watch"))
        self.assertEqual(h.commands(), [CMD_WCU_SYNC])
        self.assertEqual(h.byte("WcuSyncPending"), 1)
        h.feed(generation(rows, state(PH_READY, b"All files match GitHub 5dc0a43", 100, 0, 5)))
        self.assertEqual(h.byte("WcuSyncPending"), 0, "полный повтор пришёл")
        self.assertEqual(h.byte("WcuRefreshWanted"), 0, "второй раз не просить")

    def test_error_after_apply_refreshes_the_list_too(self) -> None:
        h = self.answering()
        rows = five_rows()
        h.feed(*rows, state(PH_READY, b"2 to update", 100, 2, 5))
        h.memory[h.symbol("WcuRefreshAfter")] = 1
        h.memory[h.symbol("WcuBusy")] = 1
        h.feed(state(PH_APPLY, b"Update WC/FILEX.WMF", 50, 1, 1),
               state(PH_ERROR, b"rename failed: CHKDSK"))
        self.assertEqual(h.byte("WcuRefreshWanted"), 1)
        self.assertEqual(h.byte("WcuBusy"), 0)
        h.run(h.symbol("Wcu_Watch"))
        self.assertEqual(h.commands(), [CMD_WCU_SYNC])
        h.feed(generation(rows, state(PH_ERROR, b"rename failed: CHKDSK")))
        self.assertEqual(h.byte("WcuSyncPending"), 0)

    def test_apply_state_alone_arms_the_refresh(self) -> None:
        """A6 потерян, и плагин не знает, что APPLY принят: события APPLY
        сами просят повтор списка после итога."""
        h = self.answering()
        h.feed(*five_rows(), state(PH_READY, b"2 to update", 100, 2, 5))
        self.assertEqual(h.byte("WcuRefreshAfter"), 0)
        h.feed(state(PH_APPLY, b"Update WC/FILEX.WMF", 50, 1, 1))
        self.assertEqual(h.byte("WcuRefreshAfter"), 1)

    def test_sync_without_result_is_repeated(self) -> None:
        """SYNC ушёл, а повтор списка потерялся целиком: через 10 с тишины
        просьба повторяется, пока не придёт полный повтор."""
        h = self.answering()
        rows = five_rows()
        h.feed(*rows[:4], state(PH_READY, b"2 to update", 100, 2, 5))
        h.run(h.symbol("Wcu_Watch"))                      # дыра — первый SYNC
        self.assertEqual(h.commands().count(CMD_WCU_SYNC), 1)
        self.at(h, 400)
        h.run(h.symbol("Wcu_Watch"))
        self.assertEqual(h.commands().count(CMD_WCU_SYNC), 1, "8 с — ещё ждём")
        self.at(h, 520)
        h.run(h.symbol("Wcu_Watch"))
        self.assertEqual(h.commands().count(CMD_WCU_SYNC), 2, "10 с без итога — снова")
        h.feed(generation(rows, state(PH_READY, b"2 to update", 100, 2, 5)))
        self.assertEqual(h.byte("WcuSyncPending"), 0)
        self.at(h, 5000)
        h.run(h.symbol("Wcu_Watch"))
        self.assertEqual(h.commands().count(CMD_WCU_SYNC), 2, "список цел — тишина норма")

    def test_ready_phase_silence_is_normal(self) -> None:
        h = self.answering()
        h.feed(*five_rows(), state(PH_READY, b"2 to update", 100, 2, 5))
        h.memory[WC_FRAMES:WC_FRAMES + 2] = (40000).to_bytes(2, "little")
        h.run(h.symbol("Wcu_Watch"))
        self.assertEqual(h.tx, bytearray())


class WcUpdateStopStartTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        require_build()

    def test_stop_is_repeated_while_task_still_runs(self) -> None:
        for answers, expected in (([0, 0, 1], 3), ([0, 0, 0], 3), ([1], 1)):
            with self.subTest(answers=answers):
                h = Harness()
                h.open_window()
                h.memory[h.symbol("WcuStarted")] = 1
                queue = list(answers)

                def esp(h: Harness) -> None:
                    if h.commands().count(CMD_WCU_STOP) > getattr(h, "seen", 0):
                        h.seen = h.commands().count(CMD_WCU_STOP)
                        h.rx += frame(RESP_ACK) + frame(RESP_WCU_STOP,
                                                        bytes([queue.pop(0)]))
                h.esp = esp
                h.run(h.symbol("PLUGIN.exit"), stack=(0x7E00,))
                self.assertEqual(h.commands().count(CMD_WCU_STOP), expected)
                self.assertEqual(h.calls[-3:], [FN_RRESB, FN_GEDPL, FN_TURBOPL])

    def test_start_answer_without_ack_still_needs_stop(self) -> None:
        """ACK START потерялся, но ответ A5 [1] пришёл: сеанс на ESP есть, и
        при выходе его нужно остановить."""
        h = Harness()
        h.open_window()

        def esp(h: Harness) -> None:
            if h.rx_polls % 40 == 0:
                h.next_frame()
            commands = h.commands()
            if CMD_WCU_START in commands and not getattr(h, "started", False):
                h.started = True
                h.rx += frame(RESP_WCU_START, b"\x01")      # без ACK
                h.pressed.add("esc")
            if CMD_WCU_STOP in commands and not getattr(h, "stopped", False):
                h.stopped = True
                h.rx += frame(RESP_ACK) + frame(RESP_WCU_STOP, b"\x01")
        h.esp = esp
        h.run(h.symbol("PLUGIN.start"), stack=(0x7E00,))
        self.assertEqual(h.byte("WcuStarted"), 1)
        self.assertIn(CMD_WCU_STOP, h.commands())


class WcUpdateSessionTest(unittest.TestCase):
    """Главный цикл целиком: проверка, A, Enter, итог, Esc и STOP."""

    @classmethod
    def setUpClass(cls) -> None:
        require_build()

    def test_check_update_exit(self) -> None:
        h = Harness()
        h.open_window()
        h.memory[h.symbol("WcuStarted")] = 1
        h.rx += b"".join(five_rows()) + state(PH_READY, b"3 to update from GitHub 5dc0a43", 100)
        stage = {"step": 0}

        def esp(h: Harness) -> None:
            if h.rx_polls % 60 == 0:
                h.next_frame()
            commands = h.commands()
            if stage["step"] == 0 and h.byte("WcuPhase") == PH_READY:
                stage["step"] = 1
                h.kbscn = ord("a")                 # отметить исполняемые
            elif stage["step"] == 1 and h.kbscn is None:
                stage["step"] = 2
                h.pressed.add("enter")             # в следующем кадре
            elif stage["step"] == 2 and CMD_WCU_APPLY in commands:
                stage["step"] = 3
                h.rx += (frame(RESP_ACK) + frame(RESP_WCU_APPLY, b"\x01") +
                         state(PH_APPLY, b"Update WC/FILEX.WMF", 10, 1, 2) +
                         frame(VFS_MKDIR, b"/WC2\0") +
                         entry(0, UPDATED, F_REMOTE | F_LOCAL, 7237, 7237, b"WC/FILEX.WMF") +
                         state(PH_APPLY, b"Update WC/TXTVIEW2.WMF", 60, 2, 2) +
                         entry(4, UPDATED, F_REMOTE | F_LOCAL, 158208, 158208,
                               b"WC/TXTVIEW2.WMF") +
                         state(PH_READY, b"1 to update from GitHub 5dc0a43", 100, 1, 5))
            elif (stage["step"] == 3 and h.byte("WcuBusy") == 0 and not h.rx and
                  CMD_WCU_SYNC in commands):
                # Итог обновления: плагин просит список заново.
                stage["step"] = 4
                rows = five_rows()
                rows[0] = entry(0, UPDATED, F_REMOTE | F_LOCAL, 7237, 7237, b"WC/FILEX.WMF")
                rows[4] = entry(4, UPDATED, F_REMOTE | F_LOCAL, 158208, 158208,
                                b"WC/TXTVIEW2.WMF")
                h.rx += (frame(RESP_ACK) + frame(RESP_WCU_SYNC, b"\x01") +
                         generation(rows, state(PH_READY, b"1 to update from GitHub 5dc0a43",
                                                100, 1, 5)))
            elif stage["step"] == 4 and not h.rx and h.byte("WcuSyncPending") == 0:
                stage["step"] = 5
                h.pressed.add("esc")
            elif stage["step"] == 5 and CMD_WCU_STOP in commands:
                stage["step"] = 6
                h.rx += frame(RESP_ACK) + frame(RESP_WCU_STOP, b"\x01")
        h.esp = esp
        h.run(h.symbol("PLUGIN.loop"), stack=(0x7E00,))
        self.assertEqual(h.machine.a, 3, "файлы менялись — WC перечитает панели")
        self.assertEqual(h.machine.ix, 0x7E00)
        self.assertIn((CMD_WCU_APPLY, bytes([0, 4])), h.frames())
        self.assertEqual(h.commands()[-1], CMD_WCU_STOP)
        self.assertEqual(stage["step"], 6, "SYNC после обновления и выход")
        self.assertLess(h.commands().index(CMD_WCU_APPLY),
                        h.commands().index(CMD_WCU_SYNC))
        self.assertEqual(h.calls[-3:], [FN_RRESB, FN_GEDPL, FN_TURBOPL])
        # Итог на экране: обновлённые строки и напоминание о перезапуске.
        status = h.line(STATUS_LINE)
        self.assertTrue(status.startswith(b"Stopping"), status)
        self.assertIn(b"1 to update from GitHub 5dc0a43 - restart WC",
                      bytes(h.text("UiStatusBuf", 70)) + b"|" + b"".join(
                          text for line, _, text in h.prints if line == STATUS_LINE))
        self.assertEqual(h.line(LIST_LINE)[58:65], b"UPDATED")
        self.assertEqual(h.line(LIST_LINE + 4)[58:65], b"UPDATED")
        self.assertEqual(h.line(LIST_LINE + 1)[1:4], b"[ ]", "текст остался неотмеченным")

    def test_start_without_ini_exits_without_stop(self) -> None:
        h = Harness()
        # Состояние прошлого запуска не должно пережить новый.
        h.memory[h.symbol("WcuCount")] = 7
        h.memory[h.symbol("UiCursor")] = 5
        h.memory[h.symbol("WcuStarted")] = 1
        h.memory[h.symbol("VfsChanged")] = 1
        h.map_c000(2)
        h.memory[0xC000:0xC010] = b"\xff" * 16
        h.map_c000(TXT)
        h.pressed = {"esc"}
        h.run(CODE, a=3)
        self.assertEqual(h.byte("WcuCount"), 0)
        self.assertEqual(h.byte("UiCursor"), 0)
        self.assertEqual(h.page(2)[:16], bytes(16), "таблица прошлого запуска стёрта")
        self.assertEqual(h.turbo, [(0, 2), (0xFF, h.turbo[-1][1])],
                         "14 МГц на время работы, при выходе — как в WC")
        self.assertEqual(h.machine.a, 0)
        self.assertNotIn(CMD_WCU_STOP, [command for command, _ in h.frames()]
                         if h.tx else [])
        self.assertTrue(h.line(STATUS_LINE).startswith(b"ERROR: "), h.line(STATUS_LINE))
        self.assertEqual(h.calls[-3:], [FN_RRESB, FN_GEDPL, FN_TURBOPL])


if __name__ == "__main__":
    unittest.main()
