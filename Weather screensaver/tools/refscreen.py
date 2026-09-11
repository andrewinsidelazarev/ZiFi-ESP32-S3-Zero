"""Эталонный рендер экрана заставки на Python.

Рисует ровно то, что должен нарисовать код Z80: теми же глифами, иконками и
координатами из design.py / build/assets.json. Тесты сравнивают кадр
эмулятора с этим эталоном; --preview генератора показывает макет глазами.
"""
import json
import struct
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import design as D  # noqa: E402

HERE = Path(__file__).resolve().parent
BUILD = HERE.parent / 'build'


class Assets:
    """Страницы данных и метаданные, как их видит плагин."""

    def __init__(self, build=BUILD):
        self.meta = json.loads((build / 'assets.json').read_text(encoding='utf-8'))
        self.pages = {n: (build / f'page{n}.bin').read_bytes() for n in (1, 2, 3)}
        self.palette = [D.cram_to_rgb(w) for w in self.meta['palette_cram']]

    def font(self, name):
        info = self.meta['fonts'][name]
        return Font(self.pages[info['page']], info['offset'])

    def glyph_at(self, page, offset):
        return Glyph(self.pages[page], offset)

    def icon(self, kind, index):
        page, offset = self.meta['icons'][kind][index]
        size = self.meta['icon_sizes'][kind]
        return self.pages[page][offset:offset + size * size], size


class Glyph:
    """Запись глифа: adv, w, h, top, left и битмап 2 бита на пиксель."""

    def __init__(self, page, offset):
        self.adv, self.w, self.h, self.top, self.left = struct.unpack_from(
            '<BBBBb', page, offset)
        stride = (self.w + 3) // 4
        self.rows = [page[offset + 5 + y * stride:offset + 5 + (y + 1) * stride]
                     for y in range(self.h)]

    def level(self, x, y):
        byte = self.rows[y][x // 4]
        return (byte >> (6 - 2 * (x % 4))) & 3


class Font:
    def __init__(self, page, offset):
        (self.line_height, self.ascent, self.first, self.last, _, _,
         self.table) = struct.unpack_from('<BBBBBBH', page, offset)
        self.page = page

    def glyph(self, code):
        if code < self.first or code > self.last:
            return None
        off = struct.unpack_from('<H', self.page, self.table + (code - self.first) * 2)[0]
        return Glyph(self.page, off) if off else None


class Screen:
    """Кадр 360x288 из индексов палитры и примитивы, зеркальные asm."""

    def __init__(self, assets):
        self.a = assets
        self.buf = bytearray(D.SCREEN_W * D.SCREEN_H)

    # --- примитивы ---------------------------------------------------------------
    def put(self, x, y, color):
        if 0 <= x < D.SCREEN_W and 0 <= y < D.SCREEN_H:
            self.buf[y * D.SCREEN_W + x] = color

    def fill_rect(self, x, y, w, h, color):
        for yy in range(y, y + h):
            self.buf[yy * D.SCREEN_W + x:yy * D.SCREEN_W + x + w] = bytes([color]) * w

    def hline(self, x, y, w, color):
        self.fill_rect(x, y, w, 1, color)

    def vline(self, x, y, h, color):
        self.fill_rect(x, y, 1, h, color)

    def background(self):
        band = D.SCREEN_H // D.PAL_BG_COUNT
        for y in range(D.SCREEN_H):
            self.hline(0, y, D.SCREEN_W, D.PAL_BG0 + min(D.PAL_BG_COUNT - 1, y // band))

    def bg_color(self, y):
        return D.PAL_BG0 + min(D.PAL_BG_COUNT - 1, y // (D.SCREEN_H // D.PAL_BG_COUNT))

    def panel(self, x, y, w, h, fill=D.PAL_PANEL, border=D.PAL_BORDER, outside=None):
        """Панель с рамкой 1 px и скруглёнными углами по маске; outside — цвет
        снаружи углов (None — градиент фона)."""
        self.fill_rect(x, y, w, h, fill)
        self.hline(x, y, w, border)
        self.hline(x, y + h - 1, w, border)
        self.vline(x, y, h, border)
        self.vline(x + w - 1, y, h, border)
        r = D.LAYOUT['PANEL_RADIUS']
        mask = self.a.meta['corner']
        for cy in range(r):
            for cx in range(r):
                cell = mask[cy][cx]
                for px, py in ((x + cx, y + cy), (x + w - 1 - cx, y + cy),
                               (x + cx, y + h - 1 - cy), (x + w - 1 - cx, y + h - 1 - cy)):
                    if cell == 0:
                        self.put(px, py, self.bg_color(py) if outside is None else outside)
                    elif cell == 1:
                        self.put(px, py, border)

    def glyph(self, x, y, g, text_set):
        """Глиф в точке (x — перо, y — верх строки); вернуть новое перо."""
        base = D.PAL_TEXT0 + text_set * 3
        for yy in range(g.h):
            for xx in range(g.w):
                level = g.level(xx, yy)
                if level:
                    self.put(x + g.left + xx, y + g.top + yy, base + level - 1)
        return x + g.adv

    def text(self, x, y, data, font, text_set, spacing=0, right=None):
        """Строка; right — правый край: символ, который закончился бы правее,
        не рисуется, и вывод на нём прекращается (как Text_Draw с TxtRight)."""
        for code in data:
            g = font.glyph(code)
            if g is None:
                continue
            if right is not None and x + g.adv > right:
                break
            x = self.glyph(x, y, g, text_set) + spacing
        return x

    def text_width(self, data, font, spacing=0):
        w = 0
        for code in data:
            g = font.glyph(code)
            if g is not None:
                w += g.adv + spacing
        return w

    def ink_bounds(self, data, font, spacing=0):
        """Первый столбец с точками и столбец за последним (как Text_InkBounds)."""
        pen, left, right = 0, None, 0
        for code in data:
            g = font.glyph(code)
            if g is None:
                continue
            if g.w:
                if left is None:
                    left = pen + g.left
                right = max(right, pen + g.left + g.w)
            pen += g.adv + spacing
        return (left if left is not None else 0), right

    def text_centered(self, x, y, width, data, font, text_set):
        """Строка по центру отрезка [x, x+width) по видимым точкам."""
        left, right = self.ink_bounds(data, font)
        return self.text(x + (width - left - right) // 2, y, data, font, text_set)

    def icon(self, x, y, kind, index):
        data, size = self.a.icon(kind, index)
        for yy in range(size):
            self.buf[(y + yy) * D.SCREEN_W + x:(y + yy) * D.SCREEN_W + x + size] = \
                data[yy * size:(yy + 1) * size]

    # --- вывод ---------------------------------------------------------------------
    def image(self):
        img = Image.new('RGB', (D.SCREEN_W, D.SCREEN_H))
        img.putdata([self.a.palette[i] for i in self.buf])
        return img


# --- форматирование чисел так же, как на Z80 ---------------------------------------
def fmt_u(value):
    return str(value).encode('ascii')


def fmt_temp(value):
    return (('-' if value < 0 else '') + str(abs(value))).encode('ascii') + b'\xF8'


def fmt_x10(value):
    """u16 x10 -> 'x.y'; ровно ноль -> '0'."""
    if value == 0:
        return b'0'
    return f'{value // 10}.{value % 10}'.encode('ascii')


def fmt_2d(value):
    return f'{value:02d}'.encode('ascii')


def weekday(year, month, day):
    """День недели 0=понедельник по формуле Зеллера (как в calendar.asm)."""
    if month < 3:
        month += 12
        year -= 1
    k = year % 100
    j = year // 100
    h = (day + (13 * (month + 1)) // 5 + k + k // 4 + j // 4 + 5 * j) % 7
    return (h + 5) % 7


def days_in_month(year, month):
    if month == 2:
        return 29 if (year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)) else 28
    return 30 if month in (4, 6, 9, 11) else 31


def prev_day(year, month, day):
    if day > 1:
        return year, month, day - 1
    if month > 1:
        return year, month - 1, days_in_month(year, month - 1)
    return year - 1, 12, 31


def next_day(year, month, day):
    if day < days_in_month(year, month):
        return year, month, day + 1
    if month < 12:
        return year, month + 1, 1
    return year + 1, 1, 1


# --- сцена ---------------------------------------------------------------------------
def render(assets, year, month, day, hour, minute, record, status=None, error=b'',
           second=0):
    """Собрать кадр. record — байты записи погоды или None; status — текст CP866
    вместо погоды (например «ZiFi не найден»), error — доклад ESP строкой ниже;
    second — секунда часов: в нечётную двоеточие скрыто."""
    L = D.LAYOUT
    s = Screen(assets)
    body = assets.font('BODY')
    caps = assets.font('CAPS')
    clock = assets.font('CLOCK')
    temp = assets.font('TEMP')
    enc = D.encode_cp866

    s.background()
    s.panel(L['PANEL_L_X'], L['PANEL_L_Y'], L['PANEL_L_W'], L['PANEL_L_H'])
    s.panel(L['PANEL_R_X'], L['PANEL_R_Y'], L['PANEL_R_W'], L['PANEL_R_H'])

    # --- левая панель: место, часы, дата ---------------------------------------
    left_right = L['PANEL_L_X'] + L['PANEL_L_W'] - 4   # LEFT_TEXT_RIGHT в screen.asm
    if record:
        s.icon(L['LOC_ICON_X'], L['LOC_ICON_Y'], 'mini', 3)
        place = record[D.REC_PLACE:D.REC_PLACE + D.REC_PLACE_LEN].split(b'\0')[0]
        s.text(L['LOC_TEXT_X'], L['LOC_TEXT_Y'], place, body,
               D.TEXT_SET_INDEX['accent_panel'], right=left_right)
    white = D.TEXT_SET_INDEX['white_panel']
    clock_x, clock_y = assets.meta['clock']['x'], assets.meta['clock']['y']
    x = s.text(clock_x, clock_y, fmt_2d(hour), clock, white)
    colon = clock.glyph(ord(':'))
    if second % 2 == 0:
        s.glyph(x, clock_y, colon, white)
    s.text(x + colon.adv, clock_y, fmt_2d(minute), clock, white)
    wday = weekday(year, month, day)
    s.text(L['DATE1_X'], L['DATE1_Y'], enc(D.WEEKDAY_FULL[wday]), body,
           D.TEXT_SET_INDEX['muted_panel'])
    s.text(L['DATE2_X'], L['DATE2_Y'],
           fmt_u(day) + b' ' + enc(D.MONTH_GEN[month - 1]) + b' ' + fmt_u(year),
           body, D.TEXT_SET_INDEX['muted_panel'])

    # --- текущая погода ------------------------------------------------------------
    if record:
        code = record[D.REC_CODE]
        icon_day, icon_night, _ = D.wmo_lookup(code)
        s.icon(L['CUR_ICON_X'], L['CUR_ICON_Y'], 'big',
               icon_day if record[D.REC_IS_DAY] else icon_night)
        t = struct.unpack_from('<b', record, D.REC_TEMP)[0]
        s.text(L['TEMP_X'], L['TEMP_Y'], fmt_temp(t) + b'C', temp,
               D.TEXT_SET_INDEX['white_panel'])
        desc = wmo_desc(code)
        s.text(L['DESC_X'], L['DESC_Y'], desc, body, D.TEXT_SET_INDEX['white_panel'])
        sh, sm = record[D.REC_SUNSET], record[D.REC_SUNSET + 1]
        s.text(L['SUNSET_X'], L['SUNSET_Y'],
               enc(D.STRINGS['SUNSET_PREFIX']) + fmt_2d(sh) + b':' + fmt_2d(sm),
               body, D.TEXT_SET_INDEX['muted_panel'])
        wind10, precip10, press = struct.unpack_from('<HHH', record, D.REC_WIND10)
        details = [
            (0, fmt_x10(precip10) + enc(D.STRINGS['UNIT_PRECIP'])),
            (1, fmt_x10(wind10) + enc(D.STRINGS['UNIT_WIND'])),
            (2, fmt_u(press) + enc(D.STRINGS['UNIT_PRESSURE'])),
        ]
        for i, (icon, text) in enumerate(details):
            y = L['DETAIL_Y0'] + i * L['DETAIL_STEP']
            s.icon(L['DETAIL_ICON_X'], y, 'mini', icon)
            s.text(L['DETAIL_TEXT_X'], y, text, body, D.TEXT_SET_INDEX['white_panel'])
    elif status:
        s.text(L['DESC_X'], L['DESC_Y'], status, body, D.TEXT_SET_INDEX['muted_panel'],
               right=left_right)
        if error:
            if error.startswith(b'weather:'):
                error = error[len(b'weather:'):]    # как Screen_ErrorText
            s.text(L['SUNSET_X'], L['SUNSET_Y'], error, body,
                   D.TEXT_SET_INDEX['muted_panel'], right=left_right)

    # --- прогноз ---------------------------------------------------------------------
    s.text(L['FC_TITLE_X'], L['FC_TITLE_Y'], enc(D.STRINGS['TITLE_FORECAST']), caps,
           D.TEXT_SET_INDEX['muted_panel'], spacing=1)
    if record:
        count = min(record[D.REC_DAY_COUNT], D.REC_MAX_DAYS)
        rows = min(L['FC_ROWS'], max(0, count - 1))
        for i in range(rows):
            off = D.REC_DAYS + (i + 1) * D.DAY_SIZE
            dday, dmonth, dwday, dcode, tmin, tmax, dprecip = struct.unpack_from(
                '<BBBBbbH', record, off)
            y = L['FC_ROW_Y0'] + i * L['FC_ROW_STEP']
            label = (enc(D.WEEKDAY_SHORT[dwday]) + b', ' + fmt_u(dday) + b' ' +
                     enc(D.MONTH_SHORT[dmonth - 1]))
            s.text(L['FC_DAY_X'], y + L['FC_DAY_DY'], label, body,
                   D.TEXT_SET_INDEX['white_panel'])
            s.icon(L['FC_ICON_X'], y + L['FC_ICON_DY'], 'small', D.wmo_lookup(dcode)[0])
            # температура прижата к правому краю: минимум серым, максимум белым
            tmin_text = fmt_temp(tmin) + enc(D.STRINGS['DOTS'])
            tmax_text = fmt_temp(tmax)
            x = L['FC_RIGHT'] - s.text_width(tmin_text, body) - s.text_width(tmax_text, body)
            x = s.text(x, y + L['FC_TEMP_DY'], tmin_text, body,
                       D.TEXT_SET_INDEX['muted_panel'])
            s.text(x, y + L['FC_TEMP_DY'], tmax_text, body, D.TEXT_SET_INDEX['white_panel'])
            rain = fmt_x10(dprecip) + enc(D.STRINGS['UNIT_MM_UPPER'])
            s.text(L['FC_RIGHT'] - s.text_width(rain, caps), y + L['FC_RAIN_DY'], rain,
                   caps, D.TEXT_SET_INDEX['rain_panel'])
            if i + 1 < rows:
                s.hline(L['FC_SEP_X0'], y + L['FC_SEP_DY'],
                        L['FC_SEP_X1'] - L['FC_SEP_X0'] + 1, D.PAL_SEP)

    # --- календарь: неделя с понедельника -------------------------------------------
    s.text(L['CAL_TITLE_X'], L['CAL_TITLE_Y'],
           enc(D.MONTH_UPPER[month - 1]) + b' ' + fmt_u(year), caps,
           D.TEXT_SET_INDEX['muted_panel'], spacing=1)
    s.panel(L['STRIP_X'], L['STRIP_Y'], L['STRIP_W'], L['STRIP_H'], fill=D.PAL_STRIP,
            border=D.PAL_STRIP, outside=D.PAL_PANEL)
    y0, m0, d0 = year, month, day
    for _ in range(wday):
        y0, m0, d0 = prev_day(y0, m0, d0)
    disc = assets.meta['disc']
    # круг под число чётной (0) и нечётной (1) ширины рисунка; сдвиг по x —
    # в поле left глифа (см. gen_assets.calendar_disc)
    discs = [assets.glyph_at(disc['page'], disc['even']),
             assets.glyph_at(disc['page'], disc['odd'])]
    for col in range(7):
        cx = L['CAL_COL_X0'] + col * L['CAL_COL_W']
        label = enc(D.WEEKDAY_SHORT[col])
        s.text_centered(cx, L['CAL_LABEL_Y'], L['CAL_COL_W'], label, caps,
                        D.TEXT_SET_INDEX['red_strip' if col >= 5 else 'muted_strip'])
        num = fmt_u(d0)
        active = (y0, m0, d0) == (year, month, day)
        if active:
            left, right = s.ink_bounds(num, body)
            s.glyph(cx, disc['y'], discs[(left + right) % 2],
                    D.TEXT_SET_INDEX['accent_strip'])
        s.text_centered(cx, L['CAL_NUM_Y'], L['CAL_COL_W'], num, body,
                        D.TEXT_SET_INDEX['dark_accent' if active else 'white_strip'])
        y0, m0, d0 = next_day(y0, m0, d0)
    return s


def wmo_desc(code):
    return D.encode_cp866(D.wmo_lookup(code)[2])


SAMPLE_RECORD = D.pack_record(
    'Roma', 12, 95, 0, 133, 239, 675, (6, 52), (19, 38), (0, 25),
    [(10, 9, 3, 3, 10, 19, 0), (11, 9, 4, 3, 10, 19, 0), (12, 9, 5, 3, 9, 19, 0),
     (13, 9, 6, 61, 11, 19, 1), (14, 9, 0, 2, 14, 21, 0), (15, 9, 1, 80, 12, 22, 18)])


def preview(path):
    assets = Assets()
    screen = render(assets, 2026, 9, 10, 0, 25, SAMPLE_RECORD)
    screen.image().save(path)
    scaled = screen.image().resize((D.SCREEN_W * 2, D.SCREEN_H * 2), Image.NEAREST)
    scaled.save(Path(path).with_name('preview_x2.png'))


if __name__ == '__main__':
    preview(BUILD / 'preview.png')
