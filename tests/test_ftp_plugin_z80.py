"""Машинные тесты FTP-плагина: код возврата для Wild Commander и CRC-16 окна.

ZIFIFTP.WMF исполняется в эмуляторе Z80 (пакет z80) с заглушкой API Wild
Commander (#6006) и портов ZiFi. Проверяется:
- после записи, удаления или создания каталога плагин при выходе возвращает 3
  (перечитать обе панели, как SMB-сервер и UNZIP), после одного чтения — 0,
  а признак изменений не переживает запуск плагина;
- табличный CRC-16/CCITT-FALSE окна совпадает с эталоном на Python и таблицы
  строятся при запуске плагина;
- даты: READDIR просит у API 58 время ИЗМЕНЕНИЯ (бит 6) и отдаёт дату/время
  хвостом за именем, STAT добавляет структуру FILEX GET_METADATA, OPEN=3 и
  SET_METADATA пишут дату в запись WC, а старый режим 2 больше не удаляет файл;
- окно центрирует PRWOW при каждом показе (X/Y=#FF, буфер фона 0);
- оба плагина между файловыми запросами ждут байтов ZiFi опросом, а не HALT:
  следующий запрос ESP разбирается сразу, без простоя до конца кадра;
- оба плагина объявляют в OPENDIR пакетный READDIR и отдают пачку записей
  отдельными кадрами с итогом [2] (каталог продолжается) или [1] (конец);
- шкала Wi-Fi в поле Status цветная: клетки и процент — зелёные от 60 %,
  жёлтые от 30 %, ниже красные (коды цвета TXTPR, модель — по исходнику WC).
Нужна сборка: "FTP Server\\build.bat".
"""
import pathlib
import random
import re
import unittest

import z80

ROOT = pathlib.Path(__file__).resolve().parents[1]
WMF = ROOT / "FTP Server" / "build" / "ZIFIFTP.WMF"
SYM = ROOT / "FTP Server" / "build" / "ZIFIFTP.sym"
# SMB-плагин отвечает на те же файловые запросы ESP тем же кодом STAT/READDIR,
# поэтому даты и окно проверяются на обоих плагинах одним стендом.
SMB_WMF = ROOT / "SMB Server" / "build" / "ZIFISMB.WMF"
SMB_SYM = ROOT / "SMB Server" / "build" / "ZIFISMB.sym"
WC_API = 0x6006
WC_FRAMES = 0x6009                      # кадровый таймер WC, сразу за JP диспетчера
API_STUB = 0x7FE0                       # тело заглушки API: в #6006 только JP, как в WC
API_BREAK = API_STUB + 5                # RET заглушки
ALT_A_CELL = 0x7FF0                     # сюда заглушка кладёт A' (параметр функции)
CODE = 0x8000
STACK = 0x5F00
SENTINEL = 0x7F00                       # адрес возврата из проверяемой процедуры
INT_HANDLER = 0x0038                    # IM1/RST 38: EI, RET
BREAKPOINT_HIT = 1 << 1
CMD_FTP_STOP = 0x07
FTP_STOP_FRAME = bytes([0x5A, CMD_FTP_STOP, 0, 0, CMD_FTP_STOP])   # SYNC,CMD,LEN,XOR
CMD_SMB_STOP = 0x0C

# API Wild Commander
FN_PRWOW, FN_RRESB, FN_PRSRW, FN_TXTPR, FN_GEDPL, FN_ESC = 1, 2, 3, 11, 15, 23
FN_STREAM, FN_FENTRY, FN_MKFILE, FN_MKDIR, FN_DELETE, FN_INT_PL = 57, 59, 72, 73, 75, 86
FN_FINDNEXT, FN_FILEX, FN_ADIR = 58, 77, 56
FILEX_QUERY_CAPS, FILEX_SET_METADATA, FILEX_GET_METADATA = 0, 6, 8
FILEX_CAP_SET_METADATA, FILEX_CAP2_GET_METADATA = 0x20, 0x01
VFS_STAT, VFS_READDIR, VFS_OPEN, VFS_SET_METADATA = 0x40, 0x42, 0x50, 0x5E
VFS_OPENDIR = 0x41
EVT_WIFI_SIGNAL = 0x66
WINDOW_COLOR, GREEN, YELLOW, RED = 0x17, 0x1C, 0x1E, 0x1A


def crc16_ccitt_false(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: полином #1021, начальное значение #FFFF."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = (crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


class Symbols:
    def __init__(self, path: pathlib.Path) -> None:
        self.text = path.read_text(encoding="utf-8", errors="replace")

    def __call__(self, name: str) -> int:
        match = re.search(rf"^{re.escape(name)}:\s+EQU\s+0x([0-9A-Fa-f]+)", self.text, re.M)
        if not match:
            raise AssertionError(f"в ZIFIFTP.sym нет символа {name}")
        return int(match.group(1), 16)


class Harness:
    """Плагин в памяти Z80: WC отвечает заглушками, ZiFi молчит."""

    def __init__(self, wmf: pathlib.Path = WMF, sym: pathlib.Path = SYM) -> None:
        data = wmf.read_bytes()
        assert data[16:32] == b"WildCommanderMDL", "это не .WMF"
        page, sectors = data[36], data[37]
        assert page == 0 and sectors, "страница кода не описана"
        self.symbol = Symbols(sym)
        self.machine = z80.Z80Machine()
        self.memory = self.machine.memory
        self.machine.set_memory_block(CODE, data[512:512 + sectors * 512])
        # В #6006 WC держит три байта JP на диспетчер, а в #6009..#600A —
        # кадровый таймер, который читает SMB-плагин. Поэтому тело заглушки
        # лежит отдельно: EX AF,AF' ; LD (ALT_A_CELL),A ; EX AF,AF' ; RET
        self.machine.set_memory_block(WC_API, bytes([0xC3, API_STUB & 0xFF, API_STUB >> 8]))
        self.machine.set_memory_block(
            API_STUB, bytes([0x08, 0x32, ALT_A_CELL & 0xFF, ALT_A_CELL >> 8, 0x08, 0xC9]))
        self.machine.set_memory_block(INT_HANDLER, bytes([0xFB, 0xC9]))
        self.machine.set_input_callback(self.port_in)
        self.machine.set_output_callback(self.port_out)
        for address in (API_BREAK, SENTINEL):
            self.machine.set_breakpoint(address)
        self.calls: list[int] = []
        self.tx = bytearray()               # байты Z80 -> ESP
        # Модель диска WC: имя -> (размер, каталог); FENTRY находит только их.
        self.files: dict[bytes, tuple[int, bool]] = {}
        # Очередь записей для FINDNEXT: (размер, дата, время, флаг, имя).
        self.dir_entries: list[tuple[int, int, int, int, bytes]] = []
        self.findnext_masks: list[int] = []
        self.filex_caps = 0
        self.filex_caps2 = 0
        self.metadata = bytes(16)           # что отдаёт GET_METADATA
        self.filex_ops: list[int] = []
        self.set_metadata_blocks: list[bytes] = []
        self.prwow_windows: list[bytes] = []
        # Экран окна по TXTPR: строка -> знаки и цвета (колонка с единицы).
        self.screen: dict[int, bytearray] = {}
        self.colors: dict[int, list[int]] = {}
        self.esc_polls = 0
        self.esc_after: int | None = None   # с какого опроса Esc считать нажатым
        self.interrupts = 0
        # Очередь ESP -> Z80. Сценарий ESP (esp) вызывается при каждом чтении
        # счётчика приёма и может дослать байты или сменить кадр.
        self.rx = bytearray()
        self.rx_polls = 0
        self.esp = None

    # --- порты ZiFi: модуль есть, приём — из очереди rx ---------------------------------
    def port_in(self, port: int) -> int:
        if port & 0xFF == 0xEF:
            high = port >> 8
            if high == 0xC7:
                return 0x01                 # ZiFi обнаружен
            if high == 0xC0:
                self.rx_polls += 1
                if self.esp is not None:
                    self.esp(self)
                return min(len(self.rx), 0xFF)
            if high == 0xC1:
                return 0xFF
            if high < 0xC0 and self.rx:
                return self.rx.pop(0)       # данные: INIR ставит в старший байт счётчик
            return 0
        return 0xFF

    def port_out(self, port: int, value: int) -> None:
        if port == 0xBFEF:
            self.tx.append(value)

    # --- API Wild Commander ------------------------------------------------------------
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
        self.calls.append(api)
        if api == FN_PRWOW:
            # Снимок дескриптора в момент вызова: по нему видно, что плагин
            # просил WC центрировать окно, а не ставил его по старым координатам.
            self.prwow_windows.append(bytes(self.memory[m.ix:m.ix + 16]))
            self.flags(zero=True)
        elif api == FN_TXTPR:
            self.txtpr(m)
        elif api in (FN_INT_PL, FN_GEDPL, FN_PRSRW, FN_RRESB, FN_STREAM, FN_ADIR):
            self.flags(zero=True)
        elif api == FN_FENTRY:
            self.fentry(m)
        elif api == FN_FINDNEXT:
            self.findnext(m)
        elif api == FN_FILEX:
            self.filex(m)
        elif api == FN_DELETE:
            self.flags(zero=True)           # удалить не удалось
        elif api in (FN_MKFILE, FN_MKDIR):
            m.a = 3
            self.flags()                    # NZ — создать не удалось
        elif api == FN_ESC:
            self.esc_polls += 1
            pressed = self.esc_after is not None and self.esc_polls >= self.esc_after
            self.flags(zero=not pressed)
        else:
            raise AssertionError(f"неожиданный вызов WC API {api} в PC=#{m.pc:04X}")
        self.ret()

    def word(self, address: int) -> int:
        return self.memory[address] | self.memory[address + 1] << 8

    def txtpr(self, m) -> None:
        """API 11 по исходнику WC (WCIF.ASM, TXTPR): D/E — строка и колонка,
        #0D — строка ниже с той же колонки и цвет окна, #0B/#0C n — сдвиг
        колонки/строки. Коды 1..8: одиночный (следом байт от 9) — чернила
        кода на фоне окна, два одинаковых — яркие (+8), два разных — фон и
        чернила (код - 1); 9 — обмен фона и чернил. #0E (центрирование)
        строк с полями не касается и здесь не моделируется."""
        window = self.memory[m.ix + 6]
        line, column, start = m.d, m.e, m.e
        attr = window
        address = m.hl
        while True:
            byte = self.memory[address]
            if byte == 0:
                break
            if byte == 0x0D:
                line, column, attr = line + 1, start, window
                address += 1
            elif byte in (0x0B, 0x0C):
                if byte == 0x0B:
                    column += self.memory[address + 1]
                else:
                    line += self.memory[address + 1]
                address += 2
            elif byte == 0x0E:
                address += 1
            elif byte == 0xFE:
                raise AssertionError("вставка строки #FE в окне плагина не ожидается")
            elif byte == 9:
                attr = ((attr << 4) | (attr >> 4)) & 0xFF
                address += 1
            elif byte < 9:
                following = self.memory[address + 1]
                if following >= 9:
                    attr = (window & 0xF0) | (byte & 7)
                    address += 1
                elif following == byte:
                    attr = (window & 0xF0) | (byte | 8)
                    address += 2
                else:
                    attr = ((byte - 1) << 4 | (following - 1)) & 0xFF
                    address += 2
            else:
                self.screen.setdefault(line, bytearray(b" " * 80))[column - 1] = byte
                self.colors.setdefault(line, [window] * 80)[column - 1] = attr
                column += 1
                address += 1
        self.flags(zero=True)

    def fentry(self, m) -> None:
        """API 59: HL -> [тип][имя,0]; NZ и размер в DE:HL, если объект есть."""
        kind = self.memory[m.hl]
        name = bytes(self.memory[m.hl + 1:m.hl + 257]).split(bytes(1), 1)[0]
        found = self.files.get(name)
        if found is None or found[1] != (kind == 0x10):
            self.flags(zero=True)
            return
        m.hl = found[0] & 0xFFFF
        m.de = found[0] >> 16
        self.flags()

    def findnext(self, m) -> None:
        """API 58: маска в A', буфер в DE; [размер][дата][время][флаг][имя,0]."""
        self.findnext_masks.append(self.memory[ALT_A_CELL])
        if not self.dir_entries:
            self.flags(zero=True)
            return
        size, date, time, flag, name = self.dir_entries.pop(0)
        record = (size.to_bytes(4, "little") + date.to_bytes(2, "little") +
                  time.to_bytes(2, "little") + bytes([flag]) + name + bytes(1))
        self.memory[m.de:m.de + len(record)] = record
        m.de = (m.de + len(record)) & 0xFFFF
        self.flags()

    def filex(self, m) -> None:
        """API 77: блок параметров по HL, статус в A и Z/NZ."""
        block = m.hl
        operation = self.memory[block + 2]
        self.filex_ops.append(operation)
        buffer = self.word(block + 8)
        if operation == FILEX_QUERY_CAPS:
            self.memory[block + 24] = self.filex_caps
            self.memory[block + 25] = self.filex_caps2
            self.memory[block + 29] = 1         # ABI 1
        elif operation == FILEX_GET_METADATA:
            if not self.filex_caps2 & FILEX_CAP2_GET_METADATA:
                raise AssertionError("GET_METADATA без объявленной возможности")
            self.memory[buffer:buffer + 16] = self.metadata
        elif operation == FILEX_SET_METADATA:
            data = bytes(self.memory[buffer:buffer + 16])
            self.set_metadata_blocks.append(data)
            self.memory[buffer + 15] = data[2]  # применённый атрибут
        else:
            raise AssertionError(f"неожиданная операция FILEX {operation}")
        self.memory[block + 28] = 0
        m.a = 0
        self.flags(zero=True)

    def frames(self) -> list[tuple[int, bytes]]:
        """Разобрать отправленное ESP: [5A][CMD][LEN_L][LEN_H][DATA][XOR]."""
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

    def run(self, pc: int, *, a: int = 0, ix: int = 0x7E00, hl: int = 0, bc: int = 0,
            stack: tuple[int, ...] = ()) -> None:
        """Выполнить код с адреса pc до возврата по адресу SENTINEL.

        stack — слова, которые процедура снимет со стека сама (например IX
        для `pop ix`), сверху вниз; под ними лежит SENTINEL."""
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
        for _ in range(200_000):
            m.ticks_to_stop = 5_000_000
            event = m.run()
            if event & BREAKPOINT_HIT and m.pc == SENTINEL:
                return
            if event & BREAKPOINT_HIT and m.pc == API_BREAK:
                self.handle_api()
                if m.pc == SENTINEL:
                    return          # процедура ушла в API хвостом: RET уже в SENTINEL
                continue
            if m.halted:
                # EI:HALT плагина — отдаём ему кадровое прерывание
                self.interrupts += 1
                m.on_handle_active_int()
                continue
            if event & BREAKPOINT_HIT:
                raise AssertionError(f"неожиданный breakpoint #{m.pc:04X}")
        raise AssertionError("Z80 не завершил процедуру")

    def set_request(self, payload: bytes) -> None:
        """Положить payload запроса ESP в ProtoBuf, как это делает Proto_Poll."""
        buffer = self.symbol("ProtoBuf")
        self.memory[buffer:buffer + len(payload)] = payload
        length = self.symbol("ProtoRxLen")
        self.memory[length] = len(payload) & 0xFF
        self.memory[length + 1] = len(payload) >> 8

    def changed(self) -> int:
        return self.memory[self.symbol("VfsChanged")]

    def next_frame(self) -> None:
        # То, что делает прерывание WC: кадровый таймер +1.
        value = (self.word(WC_FRAMES) + 1) & 0xFFFF
        self.memory[WC_FRAMES] = value & 0xFF
        self.memory[WC_FRAMES + 1] = value >> 8

    def crc_tables(self) -> tuple[bytes, bytes]:
        hi = self.symbol("VfsCrcHi")
        lo = self.symbol("VfsCrcLo")
        return bytes(self.memory[hi:hi + 256]), bytes(self.memory[lo:lo + 256])


def reference_tables() -> tuple[bytes, bytes]:
    entries = [crc16_ccitt_false(bytes([i])) ^ 0 for i in range(256)]
    # запись i таблицы — CRC восьми сдвигов значения i<<8 без начального #FFFF
    table = []
    for i in range(256):
        crc = i << 8
        for _ in range(8):
            crc = (crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
        table.append(crc)
    del entries
    return bytes(t >> 8 for t in table), bytes(t & 0xFF for t in table)


class FtpExitRefreshTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not WMF.exists() or not SYM.exists():
            raise unittest.SkipTest("нет сборки FTP Server/build: запустите build.bat")

    def test_header_version(self) -> None:
        data = WMF.read_bytes()
        self.assertEqual(data[165:197].rstrip(b" "), b"ZiFi FTP Server v0.15")
        self.assertEqual(data[197], 3, "плагин меню F10")

    def test_write_requests_mark_disk_changed(self) -> None:
        cases = [
            ("Vfs_Open", b"\x01file.txt\x00", 1, FN_MKFILE),     # STOR: замена файла
            ("Vfs_Delete", b"file.txt\x00", 1, FN_FENTRY),       # DELE
            ("Vfs_Mkdir", b"newdir\x00", 1, FN_MKDIR),           # MKD
            ("Vfs_Open", b"\x00file.txt\x00", 0, FN_FENTRY),     # RETR: только чтение
            ("Vfs_Stat", b"file.txt\x00", 0, FN_FENTRY),         # SIZE/MDTM
        ]
        for routine, payload, expected, must_call in cases:
            with self.subTest(routine=routine, payload=payload):
                h = Harness()
                h.memory[h.symbol("VfsChanged")] = 0
                h.set_request(payload)
                h.run(h.symbol(routine))
                self.assertIn(must_call, h.calls, "запрос не дошёл до файлового API WC")
                self.assertEqual(h.changed(), expected)
                self.assertTrue(h.machine.f & 1, "файловый запрос должен вернуть CF=1 (обработан)")
                self.assertTrue(h.tx, "ответ ESP не отправлен")

    def test_exit_after_changes_asks_wc_to_reread_panels(self) -> None:
        h = Harness()
        h.memory[h.symbol("VfsChanged")] = 1
        h.run(h.symbol("PLUGIN.exit"), stack=(0x7E00,))
        self.assertEqual(h.machine.a, 3, "код возврата 3 — перечитать обе панели")
        self.assertEqual(h.machine.ix, 0x7E00, "IX панели восстановлен")
        self.assertEqual(h.calls, [FN_RRESB, FN_GEDPL], "окно закрыто, экран WC возвращён (как SMB/UNZIP)")
        self.assertIn(FTP_STOP_FRAME, bytes(h.tx), "ESP не получил FTP_STOP")

    def test_exit_without_changes_returns_zero(self) -> None:
        h = Harness()
        h.memory[h.symbol("VfsChanged")] = 0
        h.run(h.symbol("PLUGIN.exit"), stack=(0x7E00,))
        self.assertEqual(h.machine.a, 0, "клиент только читал — панели не трогаем")
        self.assertEqual(h.calls, [FN_RRESB, FN_GEDPL])

    def test_stale_flag_is_reset_and_crc_tables_built_on_start(self) -> None:
        # Страница плагина остаётся в памяти WC между запусками: признак с
        # прошлого сеанса не должен вызывать перечитывание панелей в новом.
        h = Harness()
        h.memory[h.symbol("VfsChanged")] = 1
        hi = h.symbol("VfsCrcHi")
        h.memory[hi:hi + 512] = bytes(512)
        h.esc_after = 2
        h.run(CODE, a=3)                    # запуск из меню F10; zifi.ini не найден -> ошибка -> Esc
        self.assertEqual(h.calls[0], FN_INT_PL)
        self.assertEqual(h.calls[1], FN_GEDPL)
        self.assertIn(FN_PRWOW, h.calls)
        self.assertNotEqual(h.memory[h.symbol("ConfigError")], 0, "ожидалась ошибка конфигурации")
        self.assertGreater(h.interrupts, 0, "ожидание Esc идёт через HALT")
        self.assertEqual(h.calls[-2:], [FN_RRESB, FN_GEDPL])
        self.assertEqual(h.machine.a, 0)
        self.assertEqual(h.changed(), 0)
        self.assertEqual(h.crc_tables(), reference_tables(), "таблицы CRC строятся при запуске")


class FtpCrcTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not WMF.exists() or not SYM.exists():
            raise unittest.SkipTest("нет сборки FTP Server/build: запустите build.bat")

    def test_table_crc_matches_reference(self) -> None:
        h = Harness()
        h.run(h.symbol("Vfs_Crc16Init"))
        self.assertEqual(h.crc_tables(), reference_tables())
        rng = random.Random(20260912)
        for length in (1, 2, 255, 256, 257, 1024, 16 * 1024 - 32, 16 * 1024):
            with self.subTest(length=length):
                data = bytes(rng.randrange(256) for _ in range(length))
                h.memory[0xC000:0xC000 + length] = data        # окно page1, как в плагине
                h.run(h.symbol("Vfs_Crc16"), hl=0xC000, bc=length)
                self.assertEqual(h.machine.de, crc16_ccitt_false(data))

    def test_crc_of_known_vector(self) -> None:
        # Проверочный вектор CRC-16/CCITT-FALSE: "123456789" -> #29B1
        h = Harness()
        h.run(h.symbol("Vfs_Crc16Init"))
        h.memory[0xC000:0xC009] = b"123456789"
        h.run(h.symbol("Vfs_Crc16"), hl=0xC000, bc=9)
        self.assertEqual(h.machine.de, 0x29B1)


def le16(value: int) -> bytes:
    return value.to_bytes(2, "little")


def fat_date(year: int, month: int, day: int) -> int:
    return (year - 1980) << 9 | month << 5 | day


def fat_time(hour: int, minute: int, second: int) -> int:
    return hour << 11 | minute << 5 | second // 2


class FtpDatesTest(unittest.TestCase):
    """Даты между WC и ESP: листинг, STAT и запись даты через FILEX."""

    DATE = fat_date(2024, 3, 15)
    TIME = fat_time(14, 30, 20)

    @classmethod
    def setUpClass(cls) -> None:
        if not WMF.exists() or not SYM.exists():
            raise unittest.SkipTest("нет сборки FTP Server/build: запустите build.bat")

    def metadata(self) -> bytes:
        """Структура GET_METADATA: size, маска и значение атрибута, маска
        времён, доли, create time/date, access date, write time/date, атрибут."""
        return (bytes([16, 0x27, 0x20, 0x07, 0]) + le16(fat_time(9, 0, 0)) +
                le16(fat_date(2023, 1, 2)) + le16(fat_date(2024, 3, 16)) +
                le16(self.TIME) + le16(self.DATE) + bytes([0x20]))

    def test_readdir_asks_modification_time_and_sends_it_after_name(self) -> None:
        h = Harness()
        h.dir_entries.append((123456, self.DATE, self.TIME, 0x00, b"GAME.TAP"))
        h.run(h.symbol("Vfs_ReadDir"))
        # Бит 6 — время ИЗМЕНЕНИЯ, как в панели WC; плюс размер, дата, время.
        self.assertEqual(h.findnext_masks, [0x5C])
        [(command, payload)] = h.frames()
        self.assertEqual(command, VFS_READDIR)
        self.assertEqual(payload[:6], bytes([0, 0]) + (123456).to_bytes(4, "little"))
        self.assertEqual(payload[6:-5], b"GAME.TAP")
        # Ноль отделяет имя: старая прошивка на нём заканчивает C-строку.
        self.assertEqual(payload[-5:], bytes(1) + le16(self.DATE) + le16(self.TIME))

    def test_readdir_directory_keeps_flag_and_dates(self) -> None:
        h = Harness()
        h.dir_entries.append((0, self.DATE, self.TIME, 0x10, b"GAMES"))
        h.run(h.symbol("Vfs_ReadDir"))
        [(_, payload)] = h.frames()
        self.assertEqual(payload[1], 1, "признак каталога")
        self.assertEqual(payload[-4:], le16(self.DATE) + le16(self.TIME))

    def test_stat_appends_filex_metadata(self) -> None:
        h = Harness()
        h.files[b"file.txt"] = (70000, False)
        h.filex_caps, h.filex_caps2 = 0xFF, FILEX_CAP2_GET_METADATA
        h.metadata = self.metadata()
        h.set_request(b"file.txt" + bytes(1))
        h.run(h.symbol("Vfs_Stat"))
        [(command, payload)] = h.frames()
        self.assertEqual(command, VFS_STAT)
        self.assertEqual(payload, bytes([0, 0]) + (70000).to_bytes(4, "little") + self.metadata())
        self.assertEqual(h.filex_ops, [FILEX_QUERY_CAPS, FILEX_GET_METADATA])

    def test_stat_asks_filex_capabilities_once_per_run(self) -> None:
        h = Harness()
        h.files[b"file.txt"] = (1, False)
        h.filex_caps, h.filex_caps2 = 0xFF, FILEX_CAP2_GET_METADATA
        h.memory[h.symbol("VfsFilexKnown")] = 0   # как после входа в плагин
        for _ in range(2):
            h.set_request(b"file.txt" + bytes(1))
            h.run(h.symbol("Vfs_Stat"))
        self.assertEqual(h.filex_ops.count(FILEX_QUERY_CAPS), 1)

    def test_stat_without_get_metadata_keeps_old_reply(self) -> None:
        # Старый FILEX: ответ прежний, 6 байт — ESP считает даты неизвестными.
        h = Harness()
        h.files[b"file.txt"] = (5, False)
        h.filex_caps, h.filex_caps2 = 0x3F, 0
        h.set_request(b"file.txt" + bytes(1))
        h.run(h.symbol("Vfs_Stat"))
        [(_, payload)] = h.frames()
        self.assertEqual(payload, bytes([0, 0]) + (5).to_bytes(4, "little"))
        self.assertNotIn(FILEX_GET_METADATA, h.filex_ops)

    def test_stat_of_directory_carries_metadata_too(self) -> None:
        h = Harness()
        h.files[b"GAMES"] = (0, True)
        h.filex_caps, h.filex_caps2 = 0xFF, FILEX_CAP2_GET_METADATA
        h.metadata = self.metadata()
        h.set_request(b"GAMES" + bytes(1))
        h.run(h.symbol("Vfs_Stat"))
        [(_, payload)] = h.frames()
        self.assertEqual(payload[:2], bytes([0, 1]))
        self.assertEqual(payload[6:], self.metadata())

    def test_mfmt_path_writes_date_into_wc_entry(self) -> None:
        h = Harness()
        h.files[b"file.txt"] = (10, False)
        h.filex_caps, h.filex_caps2 = 0xFF, FILEX_CAP2_GET_METADATA
        h.memory[h.symbol("VfsChanged")] = 0
        h.set_request(bytes([3]) + b"file.txt" + bytes(1))
        h.run(h.symbol("Vfs_Open"))
        self.assertNotIn(FN_DELETE, h.calls, "режим 3 файл не трогает")
        self.assertNotIn(FN_MKFILE, h.calls)
        self.assertEqual(h.frames(), [(VFS_OPEN, bytes([0, 3, 0xFF]))])

        h.tx.clear()
        request = (bytes([16, 0, 0, 0x04, 0]) + bytes(6) + le16(self.TIME) +
                   le16(self.DATE) + bytes(1))
        h.set_request(request)
        h.run(h.symbol("Vfs_SetMetadata"))
        self.assertEqual(h.set_metadata_blocks, [request])
        self.assertEqual(h.frames(), [(VFS_SET_METADATA, bytes([0, 0]))])
        self.assertEqual(h.changed(), 1, "новая дата: при выходе перечитать панели")

    def test_open_mode3_refused_without_set_metadata_capability(self) -> None:
        h = Harness()
        h.files[b"file.txt"] = (10, False)
        h.filex_caps = 0xFF & ~FILEX_CAP_SET_METADATA
        h.set_request(bytes([3]) + b"file.txt" + bytes(1))
        h.run(h.symbol("Vfs_Open"))
        self.assertEqual(h.frames(), [(VFS_OPEN, bytes([1]))])

    def test_set_metadata_refused_outside_mode3(self) -> None:
        h = Harness()
        h.memory[h.symbol("VfsMode")] = 0
        h.set_request(bytes([16]) + bytes(15))
        h.run(h.symbol("Vfs_SetMetadata"))
        self.assertEqual(h.frames(), [(VFS_SET_METADATA, bytes([1]))])
        self.assertEqual(h.set_metadata_blocks, [])

    def test_unknown_open_mode_never_touches_disk(self) -> None:
        # Прежде любой ненулевой режим OPEN считался записью: на режим 2 плагин
        # УДАЛЯЛ файл и создавал пустой, а на режим 3 сделал бы то же самое.
        h = Harness()
        h.files[b"file.txt"] = (1234, False)
        h.memory[h.symbol("VfsChanged")] = 0
        for mode in (2, 4, 0xFF):
            with self.subTest(mode=mode):
                h.tx.clear()
                h.calls.clear()
                h.set_request(bytes([mode]) + b"file.txt" + bytes(1))
                h.run(h.symbol("Vfs_Open"))
                self.assertNotIn(FN_DELETE, h.calls)
                self.assertNotIn(FN_MKFILE, h.calls)
                self.assertEqual(h.frames(), [(VFS_OPEN, bytes([1]))])
        self.assertEqual(h.changed(), 0)


class FtpWindowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not WMF.exists() or not SYM.exists():
            raise unittest.SkipTest("нет сборки FTP Server/build: запустите build.bat")

    def test_window_is_centred_on_every_show(self) -> None:
        h = Harness()
        window = h.symbol("FtpWindow")
        # Как после прошлого запуска: PRWOW записал сюда координаты и буфер
        # фона, вычисленные для другого текстового режима.
        h.memory[window + 2] = 14
        h.memory[window + 3] = 3
        h.memory[window + 8] = 0x34
        h.memory[window + 9] = 0x12
        h.run(h.symbol("Ui_Open"))
        [snapshot] = h.prwow_windows
        self.assertEqual(snapshot[2:4], bytes([0xFF, 0xFF]), "X/Y=#FF — центр делает WC")
        self.assertEqual(snapshot[8:10], bytes(2), "буфер фона назначает WC")
        self.assertEqual(snapshot[4:6], bytes([52, 19]))



class SmbPluginDatesTest(unittest.TestCase):
    """Те же даты и то же окно у SMB-плагина."""

    DATE = fat_date(2024, 3, 15)
    TIME = fat_time(14, 30, 20)

    @classmethod
    def setUpClass(cls) -> None:
        if not SMB_WMF.exists() or not SMB_SYM.exists():
            raise unittest.SkipTest("нет сборки SMB Server/build: запустите build.bat")

    def harness(self) -> Harness:
        return Harness(SMB_WMF, SMB_SYM)

    def test_readdir_asks_modification_time_and_sends_it_after_name(self) -> None:
        h = self.harness()
        h.dir_entries.append((4096, self.DATE, self.TIME, 0x00, b"README.TXT"))
        h.run(h.symbol("Vfs_ReadDir"))
        self.assertEqual(h.findnext_masks, [0x5C])
        [(command, payload)] = h.frames()
        self.assertEqual(command, VFS_READDIR)
        self.assertEqual(payload[6:-5], b"README.TXT")
        self.assertEqual(payload[-5:], bytes(1) + le16(self.DATE) + le16(self.TIME))

    def test_stat_appends_filex_metadata(self) -> None:
        h = self.harness()
        h.files[b"file.txt"] = (70000, False)
        h.filex_caps, h.filex_caps2 = 0xFF, FILEX_CAP2_GET_METADATA
        meta = (bytes([16, 0x27, 0x21, 0x07, 0]) + le16(fat_time(9, 0, 0)) +
                le16(fat_date(2023, 1, 2)) + le16(fat_date(2024, 3, 16)) +
                le16(self.TIME) + le16(self.DATE) + bytes([0x21]))
        h.metadata = meta
        h.memory[h.symbol("VfsFilexKnown")] = 0
        h.set_request(b"file.txt" + bytes(1))
        h.run(h.symbol("Vfs_Stat"))
        [(_, payload)] = h.frames()
        self.assertEqual(payload, bytes([0, 0]) + (70000).to_bytes(4, "little") + meta)

    def test_stat_without_get_metadata_keeps_old_reply(self) -> None:
        h = self.harness()
        h.files[b"file.txt"] = (5, False)
        h.filex_caps, h.filex_caps2 = 0x3F, 0
        h.memory[h.symbol("VfsFilexKnown")] = 0
        h.set_request(b"file.txt" + bytes(1))
        h.run(h.symbol("Vfs_Stat"))
        [(_, payload)] = h.frames()
        self.assertEqual(payload, bytes([0, 0]) + (5).to_bytes(4, "little"))

    def test_window_is_centred_on_every_show(self) -> None:
        h = self.harness()
        window = h.symbol("SmbWindow")
        h.memory[window + 2] = 19
        h.memory[window + 3] = 8
        h.memory[window + 8] = 0x34
        h.memory[window + 9] = 0x12
        h.run(h.symbol("Ui_Open"))
        [snapshot] = h.prwow_windows
        self.assertEqual(snapshot[2:4], bytes([0xFF, 0xFF]))
        self.assertEqual(snapshot[8:10], bytes(2))


def frame(command: int, payload: bytes = b"") -> bytes:
    """Кадр ESP -> Z80: [5A][CMD][LEN_L][LEN_H][DATA][XOR]."""
    check = command ^ (len(payload) & 0xFF) ^ (len(payload) >> 8)
    for byte in payload:
        check ^= byte
    return bytes([0x5A, command]) + le16(len(payload)) + payload + bytes([check])


def signal_text(percent: int) -> bytes:
    """Строка шкалы, как её собирает прошивка (formatWifiSignal)."""
    filled = (percent * 16 + 50) // 100
    return f"Wi-Fi [{'#' * filled}{'.' * (16 - filled)}] {percent:3d}%".encode()


class WifiSignalColorTest(unittest.TestCase):
    """Шкала Wi-Fi в поле Status: клетки '#' и процент — цветом уровня
    (зелёный от 60 %, жёлтый от 30 %, ниже красный), остальное — цветом
    окна. Коды цвета места на экране не занимают, а поле очищается целиком:
    ни шкала, ни простой текст не оставляют хвостов друг друга."""

    STATUS_LINE = 3                      # заголовок, пустая строка, Status
    FIELD = 9                            # "Status : " — колонки 1..9
    PLUGINS = ((WMF, SYM, "FTP"), (SMB_WMF, SMB_SYM, "SMB"))

    @classmethod
    def setUpClass(cls) -> None:
        for wmf, sym, _ in cls.PLUGINS:
            if not wmf.exists() or not sym.exists():
                raise unittest.SkipTest("нет сборки FTP/SMB: запустите build.bat")

    def show_signal(self, h: Harness, percent: int) -> tuple[bytes, list[int]]:
        text = signal_text(percent)
        h.rx += frame(EVT_WIFI_SIGNAL, text)
        h.run(h.symbol("Vfs_Serve"))
        line = bytes(h.screen[self.STATUS_LINE])
        colors = h.colors[self.STATUS_LINE]
        self.assertEqual(line[:self.FIELD], b"Status : ")
        self.assertEqual(line[self.FIELD:self.FIELD + len(text)], text)
        return text, colors[self.FIELD:self.FIELD + len(text)]

    def expected(self, text: bytes, ink: int) -> list[int]:
        colors = []
        after = False
        for byte in text:
            colored = byte == ord("#") or (after and byte != ord(" "))
            colors.append(ink if colored else WINDOW_COLOR)
            after = after or byte == ord("]")
        return colors

    def test_levels_and_thresholds(self) -> None:
        for wmf, sym, name in self.PLUGINS:
            for percent, ink in ((100, GREEN), (60, GREEN), (59, YELLOW), (30, YELLOW),
                                 (29, RED), (3, RED), (0, RED)):
                with self.subTest(plugin=name, percent=percent):
                    h = Harness(wmf, sym)
                    text, colors = self.show_signal(h, percent)
                    self.assertEqual(colors, self.expected(text, ink))

    def test_plain_text_and_bar_do_not_leave_tails(self) -> None:
        """Длинный простой текст, затем шкала, затем снова текст: поле
        каждый раз перекрыто целиком, лишние колонки — пробелы цвета окна."""
        for wmf, sym, name in self.PLUGINS:
            with self.subTest(plugin=name):
                h = Harness(wmf, sym)
                long_text = 0x7000
                h.memory[long_text:long_text + 41] = b"E" * 40 + bytes(1)
                h.run(h.symbol("Ui_SetStatus"), hl=long_text)
                h.run(h.symbol("Ui_Draw"))
                line = bytes(h.screen[self.STATUS_LINE])
                self.assertEqual(line[self.FIELD:self.FIELD + 35], b"E" * 30 + b" " * 5)
                text, _ = self.show_signal(h, 75)
                line = bytes(h.screen[self.STATUS_LINE])
                self.assertEqual(line[self.FIELD + len(text):self.FIELD + 35],
                                 b" " * (35 - len(text)))
                stop = 0x7100
                h.memory[stop:stop + 9] = b"Stopping" + bytes(1)
                h.run(h.symbol("Ui_SetStatus"), hl=stop)
                h.run(h.symbol("Ui_Draw"))
                line = bytes(h.screen[self.STATUS_LINE])
                self.assertEqual(line[self.FIELD:self.FIELD + 35], b"Stopping" + b" " * 27)
                self.assertEqual(set(h.colors[self.STATUS_LINE][self.FIELD:self.FIELD + 35]),
                                 {WINDOW_COLOR}, "простой текст — цветом окна")

    def test_color_ends_with_the_line(self) -> None:
        """Цвет уровня не переходит на следующую строку окна (IP)."""
        h = Harness()
        self.show_signal(h, 10)
        self.assertEqual(set(h.colors[self.STATUS_LINE + 1]), {WINDOW_COLOR})

    def test_odd_string_cannot_overflow_the_field(self) -> None:
        """Строка другого вида (много чередований) — кодов цвета не больше
        пяти: поле не переполняется и не портит строку IP в шаблоне."""
        for wmf, sym, name in self.PLUGINS:
            with self.subTest(plugin=name):
                h = Harness(wmf, sym)
                field = h.symbol("UiStatusField")
                after = bytes(h.memory[field + 35:field + 45])
                odd = b"#.#.#.#.#.] 1 2 3 4 5 6 7 8 9 0 %%%%%%%%%%%%%%%"
                h.rx += frame(EVT_WIFI_SIGNAL, odd)
                h.run(h.symbol("Vfs_Serve"))
                self.assertEqual(bytes(h.memory[field + 35:field + 45]), after)
                codes = [byte for byte in h.memory[field:field + 35] if byte < 10]
                self.assertLessEqual(len(codes), 5)


class SmbPluginWaitTest(unittest.TestCase):
    STOP = CMD_SMB_STOP
    """Ожидание ESP между файловыми запросами: без HALT, не дольше кадра.

    Каталог ESP читает по одной записи за запрос, и следующий READDIR
    приходит через 1-3 мс после ответа. С HALT плагин успевал заснуть до
    следующего прерывания, и каждая запись стоила целый кадр — 20 мс."""

    @classmethod
    def setUpClass(cls) -> None:
        if not SMB_WMF.exists() or not SMB_SYM.exists():
            raise unittest.SkipTest("нет сборки SMB Server/build: запустите build.bat")

    def harness(self) -> Harness:
        return Harness(SMB_WMF, SMB_SYM)

    def test_returns_as_soon_as_a_byte_arrives(self) -> None:
        h = self.harness()

        def esp(h: Harness) -> None:
            if h.rx_polls == 30:
                h.rx += frame(VFS_READDIR)
        h.esp = esp
        h.run(h.symbol("Link_WaitRx"))
        self.assertEqual(h.rx_polls, 30, "байт замечен на том же опросе")
        self.assertEqual(h.interrupts, 0, "никакого HALT")

    def test_returns_when_the_frame_ends(self) -> None:
        h = self.harness()

        def esp(h: Harness) -> None:
            if h.rx_polls == 50:
                h.next_frame()
        h.esp = esp
        h.run(h.symbol("Link_WaitRx"))
        self.assertEqual(h.rx_polls, 50, "новый кадр — вернуться к проверке Esc")

    def test_spin_limit_without_interrupts(self) -> None:
        h = self.harness()
        h.run(h.symbol("Link_WaitRx"))
        self.assertEqual(h.rx_polls, 8192, "без прерываний ожидание всё равно конечно")

    def test_next_request_is_served_without_waiting_for_a_frame(self) -> None:
        h = self.harness()
        h.dir_entries += [(10, 0, 0, 0, b"ONE.TXT"), (20, 0, 0, 0, b"TWO.TXT")]
        h.rx += frame(VFS_READDIR)
        replied_at = []

        def esp(h: Harness) -> None:
            # ESP шлёт следующий READDIR вскоре после первого ответа Z80.
            if not replied_at and len(h.frames()) == 1:
                replied_at.append(h.rx_polls)
            if replied_at and h.rx_polls == replied_at[0] + 20:
                h.rx += frame(VFS_READDIR)
        h.esp = esp
        h.esc_after = 2                     # Esc после второго ответа
        h.run(h.symbol("PLUGIN.server_loop"), stack=(0x7E00,))
        replies = h.frames()
        self.assertEqual([command for command, _ in replies],
                         [VFS_READDIR, VFS_READDIR, self.STOP])
        self.assertEqual([payload[6:-5] for _, payload in replies[:2]],
                         [b"ONE.TXT", b"TWO.TXT"])
        self.assertEqual(h.interrupts, 0, "между запросами плагин не спал до кадра")


class SmbPluginDirBatchTest(unittest.TestCase):
    """Пакетный READDIR: один обмен «запрос — ответ» на пачку записей."""

    DATE = fat_date(2024, 3, 15)
    TIME = fat_time(14, 30, 20)

    @classmethod
    def setUpClass(cls) -> None:
        if not SMB_WMF.exists() or not SMB_SYM.exists():
            raise unittest.SkipTest("нет сборки SMB Server/build: запустите build.bat")

    def harness(self) -> Harness:
        return Harness(SMB_WMF, SMB_SYM)

    def entry(self, name: bytes, size: int = 1, flag: int = 0):
        return (size, self.DATE, self.TIME, flag, name)

    def test_opendir_announces_batch(self) -> None:
        h = self.harness()
        h.set_request(b"/" + bytes(1))
        h.run(h.symbol("Vfs_OpenDir"))
        self.assertIn(FN_ADIR, h.calls)
        # Старая прошивка читает только статус, новая — и байт возможностей.
        self.assertEqual(h.frames(), [(VFS_OPENDIR, bytes([0, 1]))])

    def test_batch_streams_entries_then_more(self) -> None:
        h = self.harness()
        h.dir_entries += [self.entry(b"ONE.TXT", 10), self.entry(b"TWO.TXT", 20),
                          self.entry(b"GAMES", 0, 0x10)]
        h.set_request(bytes([2]))
        h.run(h.symbol("Vfs_ReadDir"))
        frames = h.frames()
        self.assertEqual([command for command, _ in frames], [VFS_READDIR] * 3)
        self.assertEqual([payload[6:-5] for _, payload in frames[:2]],
                         [b"ONE.TXT", b"TWO.TXT"])
        self.assertEqual(frames[0][1][2:6], (10).to_bytes(4, "little"))
        self.assertEqual(frames[1][1][-4:], le16(self.DATE) + le16(self.TIME))
        self.assertEqual(frames[2][1], bytes([2]), "итог: пачка полна, каталог продолжается")
        self.assertTrue(h.machine.f & 1, "запрос обработан (CF=1)")
        self.assertEqual(h.findnext_masks, [0x5C, 0x5C])

        # Следующая пачка продолжает с третьей записи и видит конец каталога.
        h.tx.clear()
        h.set_request(bytes([2]))
        h.run(h.symbol("Vfs_ReadDir"))
        frames = h.frames()
        self.assertEqual(len(frames), 2)
        self.assertEqual(frames[0][1][1], 1, "признак каталога")
        self.assertEqual(frames[0][1][6:-5], b"GAMES")
        self.assertEqual(frames[1], (VFS_READDIR, bytes([1])), "итог: конец каталога")

    def test_empty_directory_batch_is_just_the_end(self) -> None:
        h = self.harness()
        h.set_request(bytes([16]))
        h.run(h.symbol("Vfs_ReadDir"))
        self.assertEqual(h.frames(), [(VFS_READDIR, bytes([1]))])

    def test_request_without_count_keeps_single_entry(self) -> None:
        # Старая прошивка шлёт READDIR без тела: одна запись, без итога.
        h = self.harness()
        h.dir_entries += [self.entry(b"ONE.TXT"), self.entry(b"TWO.TXT")]
        h.set_request(b"")
        h.run(h.symbol("Vfs_ReadDir"))
        frames = h.frames()
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0][1][6:-5], b"ONE.TXT")
        self.assertEqual(len(h.dir_entries), 1, "вторая запись осталась в каталоге")


class FtpPluginWaitTest(SmbPluginWaitTest):
    """Тот же цикл сервера без HALT — у FTP-плагина."""

    STOP = CMD_FTP_STOP

    @classmethod
    def setUpClass(cls) -> None:
        if not WMF.exists() or not SYM.exists():
            raise unittest.SkipTest("нет сборки FTP Server/build: запустите build.bat")

    def harness(self) -> Harness:
        return Harness(WMF, SYM)


class FtpPluginDirBatchTest(SmbPluginDirBatchTest):
    """Тот же пакетный READDIR — у FTP-плагина."""

    @classmethod
    def setUpClass(cls) -> None:
        if not WMF.exists() or not SYM.exists():
            raise unittest.SkipTest("нет сборки FTP Server/build: запустите build.bat")

    def harness(self) -> Harness:
        return Harness(WMF, SYM)


if __name__ == "__main__":
    unittest.main()
