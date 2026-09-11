"""Машинные тесты заставки: плагин исполняется в эмуляторе Z80 с подменой
API Wild Commander, портов часов Mr.Gluk и очередей ZiFi.

Кадр, нарисованный кодом Z80 в видеостраницах, сравнивается попиксельно с
эталоном tools/refscreen.py, который рисует тот же экран на Python по тем же
данным глифов, иконок и координат.
"""
from __future__ import annotations

import datetime
import struct
import sys
import unittest
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / 'tools'))
sys.path.insert(0, str(PROJECT.parent / 'shared' / 'weather'))
import design as D  # noqa: E402
import gen_assets  # noqa: E402
import refscreen  # noqa: E402
from harness import (FRAME_MS, PAGE_SIZE, Harness as BaseHarness, Symbols,  # noqa: E402
                     VirtualEsp, Wmf, bcd, make_frame)

WMF = PROJECT / 'build' / 'WEATHER.WMF'
SYM = PROJECT / 'build' / 'WEATHER.sym'
symbol = Symbols(SYM)


class Harness(BaseHarness):
    """Общий стенд (../shared/weather/harness.py) и экран TS-Conf: видеостраницы
    первого видеобуфера WC и палитра CRAM за окном FMADDR."""

    def __init__(self, esp: VirtualEsp, start: datetime.datetime, key_frame: int,
                 frame_ms: int = FRAME_MS):
        self.video = [bytearray(PAGE_SIZE) for _ in range(16)]
        self.video_page: int | None = None
        self.fmaddr = 0
        self.fm_windows: list[int] = []
        self.cram = bytearray(512)
        super().__init__(WMF, SYM, esp, start, key_frame, frame_ms)

    def port_out_extra(self, port: int, value: int) -> bool:
        if port != 0x15AF:
            return False
        # FMADDR (бит 4 — включить, биты 3..0 — A15..A12 окна) подключает
        # CRAM к окну. Модель z80 пишет в обычную RAM, поэтому при закрытии
        # окна палитра забирается из памяти под ним.
        if self.fmaddr & 0x10 and value == 0:
            base = (self.fmaddr & 0x0F) << 12
            self.cram[:] = self.memory[base:base + 512]
        if value:
            self.fm_windows.append(value)
        self.fmaddr = value
        return True

    def api_extra(self, api: int) -> bool:
        if api != 65:                       # видеостраница -> #C000
            return False
        page = self.alt_af_high()
        assert 0 <= page < 16, page
        self.swap_video(page)
        self.flags(zero=True)
        return True

    def swap_video(self, page: int) -> None:
        if self.video_page is not None:
            self.video[self.video_page][:] = self.memory[0xC000:0x10000]
        self.video_page = page
        self.machine.set_memory_block(0xC000, self.video[page])

    def snapshot(self):
        return bytes(self.screen())

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

    def test_ukrainian_place_name(self) -> None:
        # Индексы Украины и Беларуси ищет Nominatim, названия местные:
        # «Київ», «Мінск». Є є Ї ї Ў ў есть в шрифте (CP866 #F2..#F7), а
        # І і прошивка присылает латинскими I i. В названии — все шесть букв.
        body = self.assets.font('BODY')
        for ch in 'ЄєЇїЎў':
            glyph = body.glyph(D.encode_cp866(ch)[0])
            self.assertTrue(glyph is not None and glyph.w > 0, f'в шрифте нет буквы {ch}')
        record = bytearray(refscreen.SAMPLE_RECORD)
        name = D.encode_cp866('Київ ЄєЇ Ўў')
        record[D.REC_PLACE:D.REC_PLACE + D.REC_PLACE_LEN] = name.ljust(D.REC_PLACE_LEN, b'\0')
        record = bytes(record)
        h = Harness(VirtualEsp(record=record), self.START, key_frame=40)
        h.run()
        self.assert_frame(h, record, 'ukrainian_place')

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
        esp = VirtualEsp(record=None, error_text=b'weather:zip: not found')
        h = Harness(esp, self.START, key_frame=10)
        h.run()
        self.assert_frame(h, None, 'esp_error', status=D.encode_cp866(D.STRINGS['NO_WEATHER']),
                          error=b'weather:zip: not found')

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
