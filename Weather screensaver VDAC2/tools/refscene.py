"""Эталонная сцена заставки для VDAC2: дисплей-лист экрана на Python.

Сцена — это поток команд сопроцессора FT812: начать дисплей-лист, залить фон
градиентом, нарисовать панели, текст (каждый символ — ячейка шрифта в RAM_G),
иконки и круг сегодняшнего дня, показать кадр. Код Z80 строит тот же поток;
тесты сравнивают кадры, которые по обоим потокам нарисовал настоящий FT812
(tools/ft812emu.py), а --preview генератора показывает экран глазами.
"""
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import design as D  # noqa: E402
import eve  # noqa: E402
from eve import (BEGIN, BITMAP_HANDLE, BITMAP_LAYOUT, BITMAP_LAYOUT_H, BITMAP_SIZE,  # noqa: E402
                 BITMAP_SIZE_H, BITMAP_SOURCE, CELL, CLEAR, CLEAR_COLOR_RGB, COLOR_A,
                 COLOR_MASK, COLOR_RGB, DISPLAY, END, LINE_WIDTH, POINT_SIZE, STENCIL_FUNC,
                 STENCIL_OP, VERTEX2F, VERTEX_FORMAT)

HERE = Path(__file__).resolve().parent
BUILD = HERE.parent / 'build'
B = D.base                                  # общие с TS-Conf константы записи погоды


class Assets:
    def __init__(self, build=BUILD):
        self.meta = json.loads((build / 'assets.json').read_text(encoding='utf-8'))
        self.ramg_z = (build / 'ramg.z').read_bytes()

    def font(self, name):
        return self.meta['fonts'][name]

    def glyph(self, font, code):
        return font['table'].get(str(code))


# --- форматирование чисел так же, как на Z80 (и в TS-Conf-версии) ---------------------
def fmt_u(value):
    return str(value).encode('ascii')


def fmt_temp(value):
    return (('-' if value < 0 else '') + str(abs(value))).encode('ascii') + b'\xF8'


def fmt_x10(value):
    return b'0' if value == 0 else f'{value // 10}.{value % 10}'.encode('ascii')


def fmt_2d(value):
    return f'{value:02d}'.encode('ascii')


def weekday(year, month, day):
    """День недели 0=понедельник по формуле Зеллера (как в calendar.asm)."""
    if month < 3:
        month += 12
        year -= 1
    k, j = year % 100, year // 100
    return ((day + (13 * (month + 1)) // 5 + k + k // 4 + j // 4 + 5 * j) % 7 + 5) % 7


def days_in_month(year, month):
    if month == 2:
        return 29 if (year % 4 == 0 and year % 100 != 0) or year % 400 == 0 else 28
    return 30 if month in (4, 6, 9, 11) else 31


def prev_day(y, m, d):
    if d > 1:
        return y, m, d - 1
    if m > 1:
        return y, m - 1, days_in_month(y, m - 1)
    return y - 1, 12, 31


def next_day(y, m, d):
    if d < days_in_month(y, m):
        return y, m, d + 1
    if m < 12:
        return y, m + 1, 1
    return y + 1, 1, 1


class Scene:
    """Поток команд сопроцессора; координаты — точки экрана, вершины — в 1/16."""

    def __init__(self, assets):
        self.a = assets
        self.out = bytearray()

    def w(self, *values):
        self.out += eve.words(*values)

    # --- начало и конец кадра ----------------------------------------------------------------
    def begin(self):
        self.w(eve.CMD_DLSTART, CLEAR_COLOR_RGB(*D.BG_BOTTOM), CLEAR(1, 1, 1))
        # фон: градиент от левого верхнего угла к правому нижнему
        self.out += eve.cmd_gradient(0, 0, eve.rgb(D.BG_TOP),
                                     D.SCREEN_W - 1, D.SCREEN_H - 1, eve.rgb(D.BG_BOTTOM))
        self.w(VERTEX_FORMAT(4))
        self.setup_handles()

    def finish(self):
        self.w(DISPLAY(), eve.CMD_SWAP)
        return bytes(self.out)

    def setup_handles(self):
        """Ручки шрифтов и иконок: адрес, формат и размер ячейки. Свойства
        ручек — часть дисплей-листа, поэтому задаются в каждом кадре."""
        for f in self.a.meta['fonts'].values():
            for h in f['handles']:
                self.bitmap(h['handle'], h['source'], f['format'], f['stride'],
                            f['cell_w'], f['cell_h'])
        for ic in self.a.meta['icons'].values():
            self.bitmap(ic['handle'], ic['source'], ic['format'], ic['stride'],
                        ic['size'], ic['size'])

    def bitmap(self, handle, source, fmt, stride, width, height):
        self.w(BITMAP_HANDLE(handle), BITMAP_SOURCE(source),
               BITMAP_LAYOUT(fmt, stride, height), BITMAP_LAYOUT_H(stride, height),
               BITMAP_SIZE(eve.NEAREST, eve.BORDER, eve.BORDER, width, height),
               BITMAP_SIZE_H(width, height))

    # --- фигуры --------------------------------------------------------------------------------
    def rrect(self, x, y, w, h, r):
        """Прямоугольник со скруглением r: RECTS с полушириной линии r
        раздувает отрезок между вершинами на r во все стороны."""
        self.w(BEGIN(eve.RECTS), LINE_WIDTH(r * 16),
               VERTEX2F((x + r) * 16, (y + r) * 16),
               VERTEX2F((x + w - r) * 16, (y + h - r) * 16), END())

    def panel(self, rect, radius):
        """Панель макета: белый 5 % и рамка белый 10 % толщиной в точку.
        Трафарет не даёт залить внутренность дважды: сначала внутренность
        помечается в трафарете, рамка рисуется только вне её."""
        x, y, w, h = rect
        self.w(COLOR_MASK(0, 0, 0, 0), STENCIL_OP(eve.S_KEEP, eve.S_REPLACE),
               STENCIL_FUNC(eve.ALWAYS, 1, 255))
        self.rrect(x + 1, y + 1, w - 2, h - 2, radius - 1)
        self.w(COLOR_MASK(1, 1, 1, 1), STENCIL_OP(eve.S_KEEP, eve.S_KEEP),
               STENCIL_FUNC(eve.NOTEQUAL, 1, 255), COLOR_RGB(255, 255, 255),
               COLOR_A(D.BORDER_ALPHA))
        self.rrect(x, y, w, h, radius)
        self.w(STENCIL_FUNC(eve.EQUAL, 1, 255), COLOR_A(D.PANEL_ALPHA))
        self.rrect(x + 1, y + 1, w - 2, h - 2, radius - 1)
        self.w(STENCIL_FUNC(eve.ALWAYS, 0, 255), COLOR_A(255))

    def hline(self, x0, x1, y, alpha):
        self.w(COLOR_RGB(255, 255, 255), COLOR_A(alpha), BEGIN(eve.RECTS), LINE_WIDTH(8),
               VERTEX2F(x0 * 16, y * 16), VERTEX2F(x1 * 16, y * 16 + 16), END(), COLOR_A(255))

    def disc(self, cx16, cy16, r16, color, alpha=255):
        """Круг: точка FT812 радиуса r16 (1/16 точки), центр тоже в 1/16."""
        self.w(COLOR_RGB(*color))
        if alpha != 255:
            self.w(COLOR_A(alpha))
        self.w(BEGIN(eve.POINTS), POINT_SIZE(r16), VERTEX2F(cx16, cy16), END())
        if alpha != 255:
            self.w(COLOR_A(255))

    # --- текст и иконки ---------------------------------------------------------------------------
    def text_width(self, data, font, spacing=0):
        f = self.a.font(font)
        total = 0
        for code in data:
            g = self.a.glyph(f, code)
            if g:
                total += g['adv'] + spacing
        return total

    def ink_bounds(self, data, font):
        """Левый край первых и правый край последних видимых точек от пера."""
        f = self.a.font(font)
        pen, left, right = 0, None, 0
        for code in data:
            g = self.a.glyph(f, code)
            if not g:
                continue
            if g['right'] > g['left']:
                left = pen + g['left'] if left is None else left
                right = max(right, pen + g['right'])
            pen += g['adv']
        return (left or 0), right

    def text(self, x, base, data, font, color, spacing=0, right=None):
        """Строка data (CP866) с пером в x и базовой линией base; вернуть перо.
        Символ — ячейка шрифта: ручка (меняется, только когда нужна другая),
        номер ячейки и вершина левого верхнего угла ячейки."""
        f = self.a.font(font)
        self.w(COLOR_RGB(*color), BEGIN(eve.BITMAPS))
        handle = None
        pen = x
        for code in data:
            g = self.a.glyph(f, code)
            if not g:
                continue
            if right is not None and pen + g['adv'] > right:
                break                                   # дальше — рамка панели
            if g['right'] > g['left']:                  # у пробела нет точек
                h = f['handles'][g['cell'] // 128]['handle']
                if h != handle:
                    self.w(BITMAP_HANDLE(h))
                    handle = h
                self.w(CELL(g['cell'] % 128),
                       VERTEX2F((pen + g['dx'] - f['pad']) * 16,
                                (base - f['top'] + g['dy']) * 16))
            pen += g['adv'] + spacing
        self.w(END())
        return pen

    def text_right(self, right, base, data, font, color, spacing=0):
        return self.text(right - self.text_width(data, font, spacing), base, data, font,
                         color, spacing)

    def icon(self, x, y, kind, index, alpha=255):
        """Иконка: белый цвет не меняет её цветов; alpha < 255 — полупрозрачно."""
        ic = self.a.meta['icons'][kind]
        self.w(COLOR_RGB(255, 255, 255))
        if alpha != 255:
            self.w(COLOR_A(alpha))
        self.w(BEGIN(eve.BITMAPS), BITMAP_HANDLE(ic['handle']), CELL(index),
               VERTEX2F(x * 16, y * 16), END())
        if alpha != 255:
            self.w(COLOR_A(255))


def static_part(assets):
    """Неизменная часть кадра: начало дисплей-листа, фон, ручки, обе панели,
    заголовок прогноза, лента недели с подписями дней. Генератор кладёт этот
    поток в плагин готовым блоком (tools/gen_assets.py), Z80 передаёт его
    чипу как есть и дописывает изменяемую часть (render)."""
    L = D.LAYOUT
    enc = D.encode_cp866
    s = Scene(assets)
    s.begin()
    s.panel(L['PANEL_L'], L['PANEL_RADIUS'])
    s.panel(L['PANEL_R'], L['PANEL_RADIUS'])
    s.text(L['FC_X'], L['FC_TITLE_BASE'], enc(D.STRINGS['TITLE_FORECAST']), 'CAPS', D.MUTED,
           spacing=L['CAPS_SPACING'])
    sx, sy, sw, sh = L['STRIP']
    s.w(COLOR_RGB(0, 0, 0), COLOR_A(D.STRIP_ALPHA))
    s.rrect(sx, sy, sw, sh, L['STRIP_RADIUS'])
    s.w(COLOR_A(255))
    col_w = L['CAL_COL_W']
    for col in range(7):
        cx = L['CAL_COL_X0'] + col * col_w
        label = enc(D.WEEKDAY_SHORT[col])
        left, right = s.ink_bounds(label, 'CAPS')
        s.text(cx + (col_w - left - right) // 2, L['CAL_LABEL_BASE'], label, 'CAPS',
               D.RED if col >= 5 else D.MUTED)
    return bytes(s.out)


def clock_x(assets):
    """Перо часов: «ЧЧ:ММ» по центру левой панели (цифры одной ширины, так
    что ширина строки от времени не зависит)."""
    s = Scene(assets)
    px, py, pw, ph = D.LAYOUT['PANEL_L']
    return px + (pw - s.text_width(b'00:00', 'CLOCK')) // 2


def left_text_right():
    """Правый край текста в левой панели: дальше символы не рисуются."""
    px, py, pw, ph = D.LAYOUT['PANEL_L']
    return px + pw - D.LAYOUT['PANEL_PAD']


def render(assets, year, month, day, hour, minute, record, status=None, error=b'', second=0):
    """Поток команд сопроцессора для экрана заставки: неизменный блок и
    изменяемая часть в том же порядке, в каком её пишет Z80 (src/screen.asm)."""
    L = D.LAYOUT
    enc = D.encode_cp866
    s = Scene(assets)
    s.out += static_part(assets)
    right = left_text_right()

    # --- левая панель: место, часы, дата -------------------------------------------------
    if record:
        s.icon(L['LOC_ICON_X'], L['LOC_ICON_Y'], 'mini', 3)
        place = record[B.REC_PLACE:B.REC_PLACE + B.REC_PLACE_LEN].split(b'\0')[0]
        s.text(L['LOC_TEXT_X'], L['LOC_BASE'], place, 'BODY', D.ACCENT, right=right)
    x = s.text(clock_x(assets), L['CLOCK_BASE'], fmt_2d(hour), 'CLOCK', D.WHITE)
    # в нечётную секунду двоеточия нет, но место под него остаётся
    if second % 2 == 0:
        s.text(x, L['CLOCK_BASE'], b':', 'CLOCK', D.WHITE)
    colon = s.a.glyph(s.a.font('CLOCK'), ord(':'))
    s.text(x + colon['adv'], L['CLOCK_BASE'], fmt_2d(minute), 'CLOCK', D.WHITE)
    s.text(L['TEXT_X'], L['DATE1_BASE'], enc(D.WEEKDAY_FULL[weekday(year, month, day)]),
           'BODY', D.MUTED)
    s.text(L['TEXT_X'], L['DATE2_BASE'],
           fmt_u(day) + b' ' + enc(D.MONTH_GEN[month - 1]) + b' ' + fmt_u(year), 'BODY', D.MUTED)

    # --- текущая погода или причина её отсутствия ----------------------------------------
    if record:
        code = record[B.REC_CODE]
        icon_day, icon_night, desc = D.wmo_lookup(code)
        s.icon(L['CUR_ICON_X'], L['CUR_ICON_Y'], 'big',
               icon_day if record[B.REC_IS_DAY] else icon_night)
        t = struct.unpack_from('<b', record, B.REC_TEMP)[0]
        s.text(L['TEMP_X'], L['TEMP_BASE'], fmt_temp(t) + b'C', 'TEMP', D.WHITE)
        s.text(L['TEXT_X'], L['DESC_BASE'], enc(desc), 'BODY', D.WHITE)
        sh, sm = record[B.REC_SUNSET], record[B.REC_SUNSET + 1]
        s.text(L['TEXT_X'], L['SUNSET_BASE'],
               enc(D.STRINGS['SUNSET_PREFIX']) + fmt_2d(sh) + b':' + fmt_2d(sm), 'SMALL', D.MUTED)
        wind10, precip10, press = struct.unpack_from('<HHH', record, B.REC_WIND10)
        details = [(0, fmt_x10(precip10) + enc(D.STRINGS['UNIT_PRECIP'])),
                   (1, fmt_x10(wind10) + enc(D.STRINGS['UNIT_WIND'])),
                   (2, fmt_u(press) + enc(D.STRINGS['UNIT_PRESSURE']))]
        for i, (icon, text) in enumerate(details):
            y = L['DETAIL_Y0'] + i * L['DETAIL_STEP']
            s.icon(L['DETAIL_ICON_X'], y, 'mini', icon, alpha=D.DETAIL_ICON_ALPHA)
            s.text(L['DETAIL_TEXT_X'], y + L['DETAIL_DBASE'], text, 'SMALL', D.WHITE)
    elif status:
        s.text(L['TEXT_X'], L['DESC_BASE'], status, 'BODY', D.MUTED, right=right)
        if error:
            if error.startswith(b'weather:'):
                error = error[len(b'weather:'):]    # как Screen_ErrorText
            s.text(L['TEXT_X'], L['SUNSET_BASE'], error, 'SMALL', D.MUTED, right=right)

    # --- прогноз ------------------------------------------------------------------------------
    if record:
        count = min(record[B.REC_DAY_COUNT], B.REC_MAX_DAYS)
        rows = min(L['FC_ROWS'], max(0, count - 1))
        for i in range(rows):
            off = B.REC_DAYS + (i + 1) * B.DAY_SIZE
            dday, dmonth, dwday, dcode, tmin, tmax, dprecip = struct.unpack_from(
                '<BBBBbbH', record, off)
            y = L['FC_ROW_Y0'] + i * L['FC_ROW_STEP']
            label = (enc(D.WEEKDAY_SHORT[dwday]) + b', ' + fmt_u(dday) + b' ' +
                     enc(D.MONTH_SHORT[dmonth - 1]))
            s.text(L['FC_X'], y + L['FC_DAY_DBASE'], label, 'BODY', D.WHITE)
            s.icon(L['FC_ICON_X'], y + L['FC_ICON_DY'], 'small', D.wmo_lookup(dcode)[0])
            # температура прижата к правому краю: минимум серым, максимум белым
            tmin_text = fmt_temp(tmin) + enc(D.STRINGS['DOTS'])
            tmax_text = fmt_temp(tmax)
            x = L['FC_RIGHT'] - s.text_width(tmin_text, 'BODY') - s.text_width(tmax_text, 'BODY')
            x = s.text(x, y + L['FC_TEMP_DBASE'], tmin_text, 'BODY', D.MUTED)
            s.text(x, y + L['FC_TEMP_DBASE'], tmax_text, 'BODY', D.WHITE)
            rain = fmt_x10(dprecip) + enc(D.STRINGS['UNIT_MM'])
            s.text_right(L['FC_RIGHT'], y + L['FC_RAIN_DBASE'], rain, 'SMALL', D.RAIN)
            if i + 1 < rows:
                s.hline(L['FC_X'], L['FC_RIGHT'], y + L['FC_SEP_DY'], D.SEP_ALPHA)

    # --- календарь: заголовок месяца и числа недели с понедельника -------------------------
    s.text(L['FC_X'], L['CAL_TITLE_BASE'], enc(D.MONTH_UPPER[month - 1]) + b' ' + fmt_u(year),
           'CAPS', D.MUTED, spacing=L['CAPS_SPACING'])
    y0, m0, d0 = year, month, day
    for _ in range(weekday(year, month, day)):
        y0, m0, d0 = prev_day(y0, m0, d0)
    col_w = L['CAL_COL_W']
    day_font = s.a.font('DAY')
    for col in range(7):
        cx = L['CAL_COL_X0'] + col * col_w
        num = fmt_u(d0)
        left, right_ = s.ink_bounds(num, 'DAY')
        nx = cx + (col_w - left - right_) // 2
        active = (y0, m0, d0) == (year, month, day)
        if active:
            # центр круга — ровно в центре видимых точек числа (в 1/16 точки)
            top = min(s.a.glyph(day_font, c)['top'] for c in num)
            bottom = max(s.a.glyph(day_font, c)['bottom'] for c in num)
            cx16 = (nx * 2 + left + right_) * 8
            cy16 = (L['CAL_NUM_BASE'] * 2 + top + bottom) * 8
            r16 = L['CAL_CIRCLE_D'] * 8
            s.disc(cx16, cy16, r16 + 96, D.ACCENT, D.GLOW_ALPHA // 2)
            s.disc(cx16, cy16, r16 + 48, D.ACCENT, D.GLOW_ALPHA)
            s.disc(cx16, cy16, r16, D.ACCENT)
        s.text(nx, L['CAL_NUM_BASE'], num, 'DAY', D.DARK if active else D.WHITE)
        y0, m0, d0 = next_day(y0, m0, d0)
    return s.finish()


SAMPLE_RECORD = D.pack_record(
    'Roma', 12, 95, 0, 133, 239, 675, (6, 52), (19, 38), (0, 25),
    [(10, 9, 3, 3, 10, 19, 0), (11, 9, 4, 3, 10, 19, 0), (12, 9, 5, 3, 9, 19, 0),
     (13, 9, 6, 61, 11, 19, 1), (14, 9, 0, 2, 14, 21, 0), (15, 9, 1, 80, 12, 22, 18)])


def load_assets_into(chip, assets):
    """Ресурсы в RAM_G тем же путём, что у Z80: CMD_INFLATE и сжатый поток."""
    chip.cmd(eve.cmd_inflate(0, assets.ramg_z))
    chip.wait_idle()


def shoot(stream, assets=None):
    """Кадр FT812 по потоку команд сцены (PIL.Image 1024x768)."""
    import ft812emu
    assets = assets or Assets()
    with ft812emu.FT812() as chip:
        chip.boot()
        load_assets_into(chip, assets)
        chip.cmd(stream)
        chip.wait_idle()
        return chip.frame()


def preview(path):
    assets = Assets()
    img = shoot(render(assets, 2026, 9, 10, 0, 25, SAMPLE_RECORD), assets)
    img.save(path)


if __name__ == '__main__':
    preview(BUILD / 'preview.png')
