"""Машинные тесты FTP-плагина: код возврата для Wild Commander и CRC-16 окна.

ZIFIFTP.WMF исполняется в эмуляторе Z80 (пакет z80) с заглушкой API Wild
Commander (#6006) и портов ZiFi. Проверяется:
- после записи, удаления или создания каталога плагин при выходе возвращает 3
  (перечитать обе панели, как SMB-сервер и UNZIP), после одного чтения — 0,
  а признак изменений не переживает запуск плагина;
- табличный CRC-16/CCITT-FALSE окна совпадает с эталоном на Python и таблицы
  строятся при запуске плагина.
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
WC_API = 0x6006
API_BREAK = WC_API + 5                  # RET заглушки
ALT_A_CELL = 0x7FF0                     # сюда заглушка кладёт A' (параметр функции)
CODE = 0x8000
STACK = 0x5F00
SENTINEL = 0x7F00                       # адрес возврата из проверяемой процедуры
INT_HANDLER = 0x0038                    # IM1/RST 38: EI, RET
BREAKPOINT_HIT = 1 << 1
CMD_FTP_STOP = 0x07
FTP_STOP_FRAME = bytes([0x5A, CMD_FTP_STOP, 0, 0, CMD_FTP_STOP])   # SYNC,CMD,LEN,XOR

# API Wild Commander
FN_PRWOW, FN_RRESB, FN_PRSRW, FN_TXTPR, FN_GEDPL, FN_ESC = 1, 2, 3, 11, 15, 23
FN_STREAM, FN_FENTRY, FN_MKFILE, FN_MKDIR, FN_DELETE, FN_INT_PL = 57, 59, 72, 73, 75, 86


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

    def __init__(self) -> None:
        data = WMF.read_bytes()
        assert data[16:32] == b"WildCommanderMDL", "это не .WMF"
        page, sectors = data[36], data[37]
        assert page == 0 and sectors, "страница кода не описана"
        self.symbol = Symbols(SYM)
        self.machine = z80.Z80Machine()
        self.memory = self.machine.memory
        self.machine.set_memory_block(CODE, data[512:512 + sectors * 512])
        # Заглушка #6006: EX AF,AF' ; LD (ALT_A_CELL),A ; EX AF,AF' ; RET
        self.machine.set_memory_block(
            WC_API, bytes([0x08, 0x32, ALT_A_CELL & 0xFF, ALT_A_CELL >> 8, 0x08, 0xC9]))
        self.machine.set_memory_block(INT_HANDLER, bytes([0xFB, 0xC9]))
        self.machine.set_input_callback(self.port_in)
        self.machine.set_output_callback(self.port_out)
        for address in (API_BREAK, SENTINEL):
            self.machine.set_breakpoint(address)
        self.calls: list[int] = []
        self.tx = bytearray()               # байты Z80 -> ESP
        self.esc_polls = 0
        self.esc_after: int | None = None   # с какого опроса Esc считать нажатым
        self.interrupts = 0

    # --- порты ZiFi: модуль есть, очередь приёма пуста ---------------------------------
    def port_in(self, port: int) -> int:
        if port & 0xFF == 0xEF:
            high = port >> 8
            if high == 0xC7:
                return 0x01                 # ZiFi обнаружен
            if high == 0xC0:
                return 0                    # байтов от ESP нет
            if high == 0xC1:
                return 0xFF
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
        if api in (FN_INT_PL, FN_GEDPL, FN_PRWOW, FN_TXTPR, FN_PRSRW, FN_RRESB, FN_STREAM):
            self.flags(zero=True)
        elif api == FN_FENTRY:
            self.flags(zero=True)           # ничего не найдено
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
        self.assertEqual(data[165:197].rstrip(b" "), b"ZiFi FTP Server v0.14")
        self.assertEqual(data[197], 3, "плагин меню F10")

    def test_write_requests_mark_disk_changed(self) -> None:
        cases = [
            ("Vfs_Open", b"\x01file.txt\x00", 1, FN_MKFILE),     # STOR: замена файла
            ("Vfs_Open", b"\x02file.txt\x00", 1, FN_MKFILE),     # APPE: дозапись
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


if __name__ == "__main__":
    unittest.main()
