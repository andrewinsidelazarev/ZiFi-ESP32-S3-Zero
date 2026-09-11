"""Машинные тесты заставки для VDAC2: WEATHER2.WMF исполняется в эмуляторе Z80
на общем стенде (../shared/weather/harness.py), а порты SPI Z-контроллера
(#77, #57) подключены к настоящему FT812 — bt8xxemu.dll (tools/ft812emu.py).

Проверяется главное: поток команд, который код Z80 записал сопроцессору,
байт в байт совпадает с потоком эталона tools/refscene.py, и кадр, который по
нему нарисовал чип, совпадает с кадром эталона.
"""
from __future__ import annotations

import datetime
import struct
import sys
import time
import unittest
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / 'tools'))
sys.path.insert(0, str(PROJECT.parent / 'shared' / 'weather'))
import design as D  # noqa: E402
import eve  # noqa: E402
import ft812emu  # noqa: E402
import refscene  # noqa: E402
from harness import FRAME_MS, Harness as BaseHarness, VirtualEsp, Wmf  # noqa: E402

WMF = PROJECT / 'build' / 'WEATHER2.WMF'
SYM = PROJECT / 'build' / 'WEATHER2.sym'
REG_CMDB_WRITE = 0x302578
GEDPL_SAFE_PAGE = 4


class Harness(BaseHarness):
    """Общий стенд и FT812 платы VDAC2 за Z-контроллером.

    Z-контроллер (как в Unreal, zc.cpp): порт #77 — выбор устройства, бит 2 —
    FT812; порт #57 — обмен: запись отправляет байт, чтение возвращает байт
    прошлого обмена и запускает следующий. vdac2=False — платы нет: чип не
    отвечает, чтение даёт #FF."""

    def __init__(self, esp: VirtualEsp, start: datetime.datetime, key_frame: int,
                 frame_ms: int = FRAME_MS, vdac2: bool = True):
        self.chip = ft812emu.FT812() if vdac2 else None
        self.cfg = 0
        self.rdbuff = 0xFF
        self.transaction = bytearray()
        self.cmd_stream = bytearray()           # всё, что ушло в REG_CMDB_WRITE
        self.swap_times: list[datetime.datetime] = []   # время часов у каждого CMD_SWAP
        self.selects = 0
        super().__init__(WMF, SYM, esp, start, key_frame, frame_ms)

    def close(self):
        if self.chip:
            self.chip.close()
            self.chip = None

    # --- Z-контроллер --------------------------------------------------------------------
    def port_out_extra(self, port: int, value: int) -> bool:
        low = port & 0xFF
        if low == 0x77:
            value &= 0x1F
            if (self.cfg ^ value) & 0x04:
                self.select(bool(value & 0x04))
            self.cfg = value
            return True
        if low == 0x57:
            assert self.cfg & 0x02, 'SD-карта выбрана во время работы с FT812'
            self.rdbuff = self.xfer(value) if self.cfg & 0x04 else 0xFF
            return True
        return False

    def port_in_extra(self, port: int):
        if port & 0xFF == 0x57:
            value = self.rdbuff
            self.rdbuff = self.xfer(0xFF) if self.cfg & 0x04 else 0xFF
            return value
        return None

    def select(self, on: bool) -> None:
        if on:
            self.selects += 1
            self.transaction = bytearray()
        else:
            self.finish_transaction()
        if self.chip:
            self.chip.select(on)

    def xfer(self, value: int) -> int:
        self.transaction.append(value)
        return self.chip.xfer(value) if self.chip else 0xFF

    def finish_transaction(self) -> None:
        """Посылка в REG_CMDB_WRITE — это команды сопроцессору: собрать поток."""
        t = self.transaction
        if len(t) >= 3 and t[0] & 0xC0 == 0x80:
            addr = ((t[0] & 0x3F) << 16) | (t[1] << 8) | t[2]
            if addr == REG_CMDB_WRITE:
                assert (len(t) - 3) % 4 == 0, 'посылка в очередь — не целые слова'
                swap = struct.pack('<I', eve.CMD_SWAP)
                for i in range(3, len(t), 4):
                    if bytes(t[i:i + 4]) == swap:
                        # кадр собран по часам, прочитанным в этот же тик
                        self.swap_times.append(self.last_rtc)
                self.cmd_stream += t[3:]

    def on_frame(self) -> None:
        time.sleep(0.002)                        # чип живёт в настоящем времени

    # --- разбор потока -------------------------------------------------------------------
    def frames_in_stream(self, packed_size: int) -> list[bytes]:
        """Потоки кадров (от CMD_DLSTART до CMD_SWAP) после загрузки ресурсов."""
        s = bytes(self.cmd_stream)
        assert s[:4] == struct.pack('<I', eve.CMD_INFLATE), 'поток не начинается с CMD_INFLATE'
        pos = 8 + packed_size + (-packed_size) % 4
        words = [s[i:i + 4] for i in range(pos, len(s), 4)]
        start = struct.pack('<I', eve.CMD_DLSTART)
        frames, current = [], None
        for w in words:
            if w == start:
                current = bytearray()
            if current is not None:
                current += w
                if w == struct.pack('<I', eve.CMD_SWAP):
                    frames.append(bytes(current))
                    current = None
        return frames


def reference(assets, h: Harness, record, **kwargs) -> bytes:
    t = h.last_rtc
    return refscene.render(assets, t.year, t.month, t.day, t.hour, t.minute, record,
                           second=t.second, **kwargs)


def first_difference(a: bytes, b: bytes) -> str:
    for i in range(0, min(len(a), len(b)), 4):
        if a[i:i + 4] != b[i:i + 4]:
            return (f'слово {i // 4}: Z80 {a[i:i + 4][::-1].hex()} эталон {b[i:i + 4][::-1].hex()}, '
                    f'длины {len(a)} и {len(b)}')
    return f'длины {len(a)} и {len(b)}'


class Vdac2Tests(unittest.TestCase):
    START = datetime.datetime(2026, 9, 10, 0, 25, 30)

    def setUp(self) -> None:
        self.assets = refscene.Assets()
        self.harness: Harness | None = None

    def tearDown(self) -> None:
        if self.harness:
            self.harness.close()

    def run_saver(self, esp: VirtualEsp, key_frame: int, **kwargs) -> Harness:
        self.harness = Harness(esp, kwargs.pop('start', self.START), key_frame, **kwargs)
        self.harness.run()
        return self.harness

    def assert_last_frame(self, h: Harness, record, name: str, **kwargs) -> None:
        frames = h.frames_in_stream(self.assets.meta['packed_size'])
        self.assertTrue(frames, 'Z80 не собрал ни одного кадра')
        expected = reference(self.assets, h, record, **kwargs)
        actual = frames[-1]
        self.assertEqual(actual, expected, 'поток кадра расходится с эталоном: '
                         + first_difference(actual, expected))
        # кадр, нарисованный чипом по потоку Z80, и кадр по потоку эталона
        img = h.chip.frame()
        img.save(PROJECT / 'build' / f'test_{name}.png')
        h.chip.cmd(expected)
        h.chip.wait_idle()
        ref = h.chip.frame()
        self.assertEqual(img.tobytes(), ref.tobytes(), 'кадр FT812 отличается от эталона')

    def test_wmf_header(self) -> None:
        wmf = Wmf(WMF.read_bytes())
        self.assertEqual(wmf.version, 0x0A)
        self.assertEqual(wmf.kind, 0x02, 'тип заставки')
        self.assertEqual(wmf.page_count, 5)
        self.assertEqual([p for p, n in wmf.blocks if n], [0, 1, 2, 3, 4])
        self.assertEqual(wmf.consumed, wmf.size, 'в файле нет лишних байтов')
        self.assertTrue(wmf.name.startswith(b'ZiFi Weather 2 (VDAC2)'))

    def test_full_screen_matches_reference(self) -> None:
        h = self.run_saver(VirtualEsp(record=refscene.SAMPLE_RECORD), key_frame=60)
        self.assertEqual(h.mode_history, [0x04], 'экран FT812 включается через GVmod #04')
        self.assert_last_frame(h, refscene.SAMPLE_RECORD, 'full_screen')
        self.assertEqual(h.bank_history[-1], 0, 'при выходе — экран WC (MNGV_PL 0)')
        self.assertEqual(h.gedpl_pages[-1], GEDPL_SAFE_PAGE, 'GEDPL портит только пустую страницу')
        self.assertEqual(h.wait_release, 2)

    def test_colon_hidden_on_odd_second(self) -> None:
        start = datetime.datetime(2026, 9, 10, 0, 25, 30, 500000)
        h = self.run_saver(VirtualEsp(record=refscene.SAMPLE_RECORD), key_frame=40,
                           start=start)
        self.assertEqual(h.last_rtc.second % 2, 1, 'тест должен кончиться в нечётную секунду')
        self.assert_last_frame(h, refscene.SAMPLE_RECORD, 'colon_hidden')

    def test_without_zifi_clock_and_status(self) -> None:
        h = self.run_saver(VirtualEsp(present=False), key_frame=30)
        self.assert_last_frame(h, None, 'no_zifi', status=D.encode_cp866(D.STRINGS['NO_ZIFI']))

    def test_esp_error_shown(self) -> None:
        esp = VirtualEsp(records=[None], error_text=b'weather:meteo: http 503')
        h = self.run_saver(esp, key_frame=40)
        self.assert_last_frame(h, None, 'esp_error',
                               status=D.encode_cp866(D.STRINGS['NO_WEATHER']),
                               error=b'weather:meteo: http 503')

    def test_day_change_redraws_calendar_and_refreshes(self) -> None:
        # 30 сентября -> 1 октября: другой месяц в заголовке, в круге «1»
        # (рисунок нечётной ширины), погода запрашивается заново.
        start = datetime.datetime(2026, 9, 30, 23, 59, 59)
        esp = VirtualEsp(record=refscene.SAMPLE_RECORD)
        h = self.run_saver(esp, key_frame=120, start=start)
        self.assertEqual((h.last_rtc.month, h.last_rtc.day), (10, 1))
        self.assertEqual(esp.weather_requests, 2, 'новый день — новый прогноз')
        self.assert_last_frame(h, refscene.SAMPLE_RECORD, 'new_day')

    def test_loading_while_esp_is_slow(self) -> None:
        # ESP думает дольше, чем идёт тест: на экране «Запрос погоды…»
        esp = VirtualEsp(record=refscene.SAMPLE_RECORD, reply_delay=500)
        h = self.run_saver(esp, key_frame=80)
        self.assert_last_frame(h, None, 'loading',
                               status=D.encode_cp866(D.STRINGS['LOADING']))

    def test_retry_after_503_clears_error(self) -> None:
        # Первый ответ — 503: на экране причина. Через 10 секунд повтор:
        # причина стирается («Запрос погоды…»), затем приходит погода.
        esp = VirtualEsp(records=[None, refscene.SAMPLE_RECORD],
                         error_text=b'weather:meteo: http 503', reply_delay=25)
        h = self.run_saver(esp, key_frame=700)
        self.assertEqual(esp.weather_requests, 2)
        frames = h.frames_in_stream(self.assets.meta['packed_size'])
        self.assertEqual(len(frames), len(h.swap_times))
        status = {'error': D.encode_cp866(D.STRINGS['NO_WEATHER']),
                  'loading': D.encode_cp866(D.STRINGS['LOADING'])}
        seen = []
        for frame, t in zip(frames, h.swap_times):
            args = (self.assets, t.year, t.month, t.day, t.hour, t.minute)
            if frame == refscene.render(*args, None, status=status['error'],
                                        error=b'weather:meteo: http 503', second=t.second):
                kind = 'error'
            elif frame == refscene.render(*args, None, status=status['loading'],
                                          second=t.second):
                kind = 'loading'
            elif frame == refscene.render(*args, refscene.SAMPLE_RECORD, second=t.second):
                kind = 'weather'
            else:
                self.fail(f'кадр {t} не совпал ни с одним эталоном')
            if not seen or seen[-1] != kind:
                seen.append(kind)
        self.assertEqual(seen, ['loading', 'error', 'loading', 'weather'])

    def test_ukrainian_place_name(self) -> None:
        # Индексы Украины и Беларуси ищет Nominatim, названия местные:
        # «Київ», «Мінск». Є є Ї ї Ў ў есть в шрифтах (CP866 #F2..#F7), а
        # І і прошивка присылает латинскими I i. В названии — все шесть букв.
        for name in ('BODY', 'SMALL'):
            font = self.assets.font(name)
            for ch in 'ЄєЇїЎў':
                glyph = self.assets.glyph(font, D.encode_cp866(ch)[0])
                self.assertIsNotNone(glyph, f'в шрифте {name} нет буквы {ch}')
        record = bytearray(refscene.SAMPLE_RECORD)
        place = D.encode_cp866('Київ ЄєЇ Ўў')
        record[D.REC_PLACE:D.REC_PLACE + D.REC_PLACE_LEN] = place.ljust(D.REC_PLACE_LEN, b'\0')
        record = bytes(record)
        h = self.run_saver(VirtualEsp(record=record), key_frame=60)
        self.assert_last_frame(h, record, 'ukrainian_place')

    def test_second_run_after_gedpl(self) -> None:
        # Страницы плагина живут в памяти между запусками, а GEDPL при выходе
        # портит #1000..#1447 страницы в окне #0000. Второй запуск обязан
        # нарисовать тот же кадр: таблицы шрифтов (страница 1) целы.
        esp = VirtualEsp(record=refscene.SAMPLE_RECORD)
        h = self.run_saver(esp, key_frame=50)
        h.key_frame = h.frames + 50
        h.cmd_stream = bytearray()
        h.run()
        self.assertEqual(h.gedpl_pages, [GEDPL_SAFE_PAGE, GEDPL_SAFE_PAGE])
        self.assert_last_frame(h, refscene.SAMPLE_RECORD, 'second_run')

    def test_without_vdac2_exits_at_once(self) -> None:
        h = self.run_saver(VirtualEsp(record=refscene.SAMPLE_RECORD), key_frame=5000,
                           vdac2=False)
        self.assertEqual(h.mode_history, [], 'без платы экран WC не трогаем')
        self.assertFalse(h.esp.commands, 'без экрана погоду не спрашиваем')
        self.assertLess(h.frames, 200)
        self.assertEqual(h.wait_release, 2)


if __name__ == '__main__':
    unittest.main()
