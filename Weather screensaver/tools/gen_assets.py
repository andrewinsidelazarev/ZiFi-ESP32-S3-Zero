"""Генератор ресурсов заставки погоды для страниц данных плагина.

Из системных шрифтов Segoe UI и Tahoma растрируются четыре шрифта (2 бита на
пиксель: прозрачно и три уровня яркости; при mono — только прозрачно и цвет
букв, см. FONTS в design.py), из Segoe UI Emoji — иконки погоды
(8 бит на пиксель, индексы общей палитры). Результат:

  build/page1.bin .. page3.bin  — страницы данных, INCBIN в .WMF;
  src/assets.inc                — адреса шрифтов и таблицы иконок;
  src/palette.inc               — 256 слов палитры TS-Conf;
  src/strings.inc               — строки CP866 и таблицы WMO;
  src/layout.inc                — координаты раскладки из design.py;
  build/assets.json             — то же для Python-эталона (refscreen.py).

Запуск: python tools/gen_assets.py [--preview]
"""
import argparse
import json
import math
import struct
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import design as D  # noqa: E402

HERE = Path(__file__).resolve().parent
PROJ = HERE.parent
BUILD = PROJ / 'build'
SRC = PROJ / 'src'
PAGE_SIZE = 16384
GLYPH_HEADER = 5      # advance, width, height, top, left
FONT_HEADER = 8


def charset(spec):
    """Список кодов CP866 для набора символов шрифта."""
    if spec == 'ascii+cyr+extra':
        codes = (list(range(0x20, 0x7F)) + list(range(0x80, 0xB0)) +
                 list(range(0xE0, 0xF2)) + [0xF8, 0xF9, 0xFA])
    elif spec == 'ascii+cyrupper+extra':
        codes = list(range(0x20, 0x7F)) + list(range(0x80, 0xA0)) + [0xF0, 0xF8]
    else:
        codes = [D.encode_cp866(ch)[0] for ch in spec]
    return sorted(set(codes))


def decode_code(code):
    """Код CP866 (с нашими добавками) -> символ Python для растеризации."""
    if code == 0xF9:
        return '…'
    if code == 0xFA:
        return '•'
    return bytes([code]).decode('cp866')


def coverage_level(v):
    """Покрытие 0..255 -> уровень 0..3 (0 прозрачно, 3 сплошной цвет)."""
    return min(3, (v + 42) // 85)


def pack_bitmap_2bpp(img):
    """Изображение 'L' -> строки по 2 бита на пиксель, старшие биты первыми."""
    w, h = img.size
    stride = (w + 3) // 4
    px = img.load()
    rows = bytearray()
    for y in range(h):
        row = bytearray(stride)
        for x in range(w):
            row[x // 4] |= coverage_level(px[x, y]) << (6 - 2 * (x % 4))
        rows += row
    return bytes(rows), stride


def rasterize_font(name, file, size, spec, mono=False):
    """Растрировать шрифт: словарь глифов code -> (adv, w, h, top, left, data).

    mono=True — чёткое одноцветное начертание по хинтингу шрифта (как текст
    Windows без сглаживания): каждая точка либо цвет букв, либо фон. На
    экране 360x288 сглаженные полутона выглядят размыто, «как при
    близорукости», — так сказал пользователь, посмотрев на настоящем Evo."""
    font = ImageFont.truetype(f'{D.FONT_DIR}/{file}', size)
    mode = '1' if mono else ''
    ascent, descent = font.getmetrics()
    glyphs = {}
    for code in charset(spec):
        ch = decode_code(code)
        adv = int(round(font.getlength(ch, mode=mode)))
        x0, y0, x1, y1 = font.getbbox(ch, mode=mode, anchor='ls')
        w, h = x1 - x0, y1 - y0
        if ch == ' ' or w <= 0 or h <= 0:
            glyphs[code] = (adv, 0, 0, 0, 0, b'')
            continue
        img = Image.new('L', (w, h), 0)
        draw = ImageDraw.Draw(img)
        if mono:
            draw.fontmode = '1'            # без сглаживания, с хинтингом
        draw.text((-x0, -y0), ch, font=font, fill=255, anchor='ls')
        # Рамка getbbox включает всю ширину шага символа; прозрачные поля по
        # краям только занимают страницу и время вывода, поэтому обрезаем их
        # до точек, которые после сглаживания действительно видны.
        ink = img.point(lambda v: 255 if coverage_level(v) else 0).getbbox()
        if ink is None:
            glyphs[code] = (adv, 0, 0, 0, 0, b'')
            continue
        img = img.crop(ink)
        x0 += ink[0]
        y0 += ink[1]
        w, h = img.size
        data, _ = pack_bitmap_2bpp(img)
        top = ascent + y0
        assert 0 <= top < 128 and w < 128 and h < 128 and -128 <= x0 < 128, (name, ch)
        glyphs[code] = (adv, w, h, top, x0, data)
    if name == 'CLOCK':
        tabular_digits(glyphs)
        center_colon(glyphs)
    return {'name': name, 'line_height': ascent + descent, 'ascent': ascent,
            'glyphs': glyphs}


def tabular_digits(glyphs):
    """Все цифры часов — одной ширины, рисунок посередине своей клетки.
    В Segoe UI «1» уже «0», и без этого часы «11:11» и «00:00» были бы разной
    ширины, а центрированные часы прыгали бы при смене минут."""
    digits = [ord(c) for c in '0123456789']
    cell = max(glyphs[code][0] for code in digits)
    for code in digits:
        adv, w, h, top, left, data = glyphs[code]
        glyphs[code] = (cell, w, h, top, (cell - w) // 2, data)


def clock_geometry(font):
    """Перо и прямоугольник часов «ЧЧ:ММ»: по центру левой панели, верх цифр —
    LAYOUT['CLOCK_DIGIT_TOP']. Прямоугольник стирается раз в минуту."""
    L = D.LAYOUT
    glyphs = font['glyphs']
    digits = [glyphs[ord(c)] for c in '0123456789']
    cell = digits[0][0]
    width = 4 * cell + glyphs[ord(':')][0]
    top = min(g[3] for g in digits)
    bottom = max(g[3] + g[2] for g in digits)
    x = L['PANEL_L_X'] + L['PANEL_L_W'] // 2 - width // 2
    y = L['CLOCK_DIGIT_TOP'] - top
    return {'x': x, 'y': y, 'box': [x, y + top, width, bottom - top]}


def center_colon(glyphs):
    """В Segoe UI двоеточие стоит на высоте строчных букв, ниже середины цифр.
    Для часов его точки ставятся по вертикальному центру цифр."""
    colon = ord(':')
    digit = glyphs[ord('0')]
    adv, w, h, top, left, data = glyphs[colon]
    digit_center2 = 2 * digit[3] + digit[2]         # удвоенный центр: без дробей
    top = (digit_center2 - h + 1) // 2
    glyphs[colon] = (adv, w, h, top, left, data)


def pack_font(font, base):
    """Шрифт -> байты страницы, начиная со смещения base.

    Формат: заголовок 8 байт (высота строки, ascent, первый и последний код,
    2 резерва, смещение таблицы), таблица u16-смещений глифов по коду
    (0 — глифа нет), затем записи глифов: adv, w, h, top, left, битмап."""
    codes = sorted(font['glyphs'])
    first, last = codes[0], codes[-1]
    table_off = base + FONT_HEADER
    data_off = table_off + (last - first + 1) * 2
    table = bytearray()
    body = bytearray()
    index = {}
    for code in range(first, last + 1):
        g = font['glyphs'].get(code)
        if g is None:
            table += struct.pack('<H', 0)
            continue
        adv, w, h, top, left, data = g
        off = data_off + len(body)
        index[code] = off
        table += struct.pack('<H', off)
        body += struct.pack('<BBBBb', adv, w, h, top, left) + data
    header = struct.pack('<BBBBBBH', font['line_height'], font['ascent'],
                         first, last, 0, 0, table_off)
    return bytes(header) + bytes(table) + bytes(body), index


def render_emoji(font_path, ch, box):
    """Эмодзи на подложке PANEL, вписанный в квадрат box x box."""
    font = ImageFont.truetype(font_path, 109)
    canvas = Image.new('RGBA', (320, 320), D.PANEL + (255,))
    ImageDraw.Draw(canvas).text((60, 60), ch, font=font, embedded_color=True)
    rgb = canvas.convert('RGB')
    diff = ImageChops.difference(rgb, Image.new('RGB', rgb.size, D.PANEL))
    bbox = diff.convert('L').point(lambda v: 255 if v else 0).getbbox()
    assert bbox, f'эмодзи {ch!r} не отрисовался'
    content = rgb.crop(bbox)
    inner = box - 2
    scale = min(inner / content.width, inner / content.height)
    nw = max(1, int(round(content.width * scale)))
    nh = max(1, int(round(content.height * scale)))
    small = content.resize((nw, nh), Image.LANCZOS)
    out = Image.new('RGB', (box, box), D.PANEL)
    out.paste(small, ((box - nw) // 2, (box - nh) // 2))
    return out


def disc_image(width, height, cx, cy, radius, samples=16):
    """Сглаженный круг радиуса radius с центром (cx, cy) в картинке width x
    height. Координаты дробные: 0 — левый (верхний) край первой точки, так что
    центр можно поставить и на границу точек, и на середину точки. Каждая
    точка получает долю своей площади внутри круга (0..255): она считается по
    samples x samples подточкам. Подточки расположены симметрично, поэтому
    круг с центром на границе или середине точки выходит строго симметричным."""
    img = Image.new('L', (width, height), 0)
    px = img.load()
    r2 = radius * radius
    total = samples * samples
    for y in range(height):
        for x in range(width):
            inside = 0
            for sy in range(samples):
                dy = y + (sy + 0.5) / samples - cy
                for sx in range(samples):
                    dx = x + (sx + 0.5) / samples - cx
                    if dx * dx + dy * dy <= r2:
                        inside += 1
            px[x, y] = (inside * 255 + total // 2) // total
    return img


def calendar_disc(font):
    """Круги сегодняшнего дня в календаре: центр круга — ровно в центре
    рисунка цифр числа, по обеим осям.

    По высоте цифры занимают строки [top, bottom) от верха строки CAL_NUM_Y,
    центр — посередине. По ширине число ставит Text_DrawCentered: перо =
    x столбца + (CAL_COL_W - левый - правый) // 2, где левый и правый — края
    видимых точек. Отсюда центр рисунка зависит только от чётности его
    ширины: у «10» (12 точек) он на середине столбца, у «21» (13 точек) — на
    полточки левее. Поэтому кругов два, для чётной и нечётной ширины. Сдвиг
    круга от x столбца записан в поле left глифа, верх обоих кругов —
    L_CAL_CIRCLE_Y. Вернуть {'even': глиф, 'odd': глиф, 'y': верх}."""
    L = D.LAYOUT
    glyphs = font['glyphs']
    digits = [glyphs[ord(c)] for c in '0123456789']
    top = min(g[3] for g in digits)
    bottom = max(g[3] + g[2] for g in digits)
    radius = L['CAL_CIRCLE_D'] / 2
    cy = L['CAL_NUM_Y'] + (top + bottom) / 2         # центр цифр на экране
    box_y = math.floor(cy - radius)
    box_h = math.ceil(cy + radius) - box_y
    col = L['CAL_COL_W']
    result = {'y': box_y}
    for name, parity in (('even', 0), ('odd', 1)):
        # центр рисунка ширины этой чётности относительно x столбца (перо —
        # целочисленное деление с округлением вниз, как SRA/RR в text.asm)
        cx = (col - parity) // 2 + parity / 2
        box_x = math.floor(cx - radius)
        box_w = math.ceil(cx + radius) - box_x
        img = disc_image(box_w, box_h, cx - box_x, cy - box_y, radius)
        data, _ = pack_bitmap_2bpp(img)
        assert -128 <= box_x < 128
        result[name] = struct.pack('<BBBBb', col, box_w, box_h, 0, box_x) + data
    return result


def make_corner(radius):
    """Маска скруглённого угла панели radius x radius: 0 фон, 1 рамка, 2 заливка."""
    size = radius * 4
    big = Image.new('L', (size * 8, size * 8), 0)
    d = ImageDraw.Draw(big)
    d.rounded_rectangle((0, 0, size * 8 * 2, size * 8 * 2), radius=radius * 8,
                        fill=128, outline=255, width=8)
    img = big.resize((size, size), Image.BOX)
    px = img.load()
    mask = []
    for y in range(radius):
        row = []
        for x in range(radius):
            v = px[x, y]
            row.append(0 if v < 40 else (1 if v > 180 else 2))
        mask.append(row)
    return mask


def build_palette(icon_palette):
    """Собрать 256 цветов RGB по правилам design.py. Плагин загружает только
    цвета 0..239: 240..255 заняты текстовым экраном Wild Commander."""
    assert D.PAL_TEXT0 + len(D.TEXT_SETS) * 3 <= D.PAL_ICONS
    assert D.PAL_ICONS + D.PAL_ICON_COLORS <= 240
    pal = [(0, 0, 0)] * 256
    for i in range(D.PAL_BG_COUNT):
        k = i / (D.PAL_BG_COUNT - 1)
        pal[i] = tuple(int(round(D.BG_TOP[c] + (D.BG_BOTTOM[c] - D.BG_TOP[c]) * k))
                       for c in range(3))
    for idx, rgb in ((D.PAL_PANEL, D.PANEL), (D.PAL_BORDER, D.BORDER),
                     (D.PAL_STRIP, D.STRIP), (D.PAL_SEP, D.SEP),
                     (D.PAL_ACCENT, D.ACCENT), (D.PAL_WHITE, D.WHITE),
                     (D.PAL_MUTED, D.MUTED), (D.PAL_DARK, D.DARK)):
        pal[idx] = rgb
    for si, (_, ink, paper) in enumerate(D.TEXT_SETS):
        for level in (1, 2, 3):
            pal[D.PAL_TEXT0 + si * 3 + level - 1] = D.blend(ink, paper, level)
    for i, rgb in enumerate(icon_palette):
        pal[D.PAL_ICONS + i] = rgb
    return pal


def quantize_icons(images):
    """Общая квантованная палитра иконок; вернуть (палитра, список байтов)."""
    sheet_w = sum(img.width for img in images)
    sheet_h = max(img.height for img in images)
    sheet = Image.new('RGB', (sheet_w, sheet_h), D.PANEL)
    x = 0
    for img in images:
        sheet.paste(img, (x, 0))
        x += img.width
    q = sheet.quantize(colors=D.PAL_ICON_COLORS, method=Image.Quantize.MEDIANCUT,
                       dither=Image.Dither.NONE)
    qpal = q.getpalette()[:D.PAL_ICON_COLORS * 3]
    palette = [tuple(qpal[i * 3:i * 3 + 3]) for i in range(D.PAL_ICON_COLORS)]
    qpx = q.load()
    spx = sheet.load()
    result = []
    x = 0
    for img in images:
        data = bytearray()
        for y in range(img.height):
            for xx in range(img.width):
                if spx[x + xx, y] == D.PANEL:
                    data.append(D.PAL_PANEL)
                else:
                    data.append(D.PAL_ICONS + qpx[x + xx, y])
        result.append(bytes(data))
        x += img.width
    return palette, result


# GEDPL (API 15, его же вызывает MNGV_PL с нулём) пишет палитру через окно
# FMADDR на #1000 и портит #1000..#1447 страницы, подключённой к #0000. Перед
# выходом плагин подключает туда страницу GEDPL_SAFE_PAGE, у которой этот
# участок пуст, — иначе испортились бы шрифты, и следующий запуск заставки
# рисовал бы битые глифы.
GEDPL_HOLE = (0x1000, 0x1448)
GEDPL_SAFE_PAGE = 3


class Page:
    """Страница данных 16 КиБ; ресурсы кладутся подряд, минуя дыру hole."""

    def __init__(self, number, hole=None):
        self.number = number
        self.data = bytearray()
        self.hole = hole

    def _start(self, size):
        start = len(self.data)
        if self.hole and start < self.hole[1] and start + size > self.hole[0]:
            start = self.hole[1]
        return start

    def free_for(self, size):
        return self._start(size) + size <= PAGE_SIZE

    def add(self, blob):
        assert self.free_for(len(blob)), f'страница {self.number} переполнена'
        off = self._start(len(blob))
        self.data += bytes(off - len(self.data))
        self.data += blob
        return off


def asm_bytes(data):
    return ','.join(f'#{b:02X}' for b in data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--preview', action='store_true',
                        help='после генерации нарисовать build/preview.png')
    args = parser.parse_args()
    BUILD.mkdir(exist_ok=True)

    # --- шрифты: всегда в странице 1 (FONT_PAGE) ------------------------------
    page1 = Page(1)
    fonts = {}
    font_info = {}
    for name, (file, size, spec, mono) in D.FONTS.items():
        font = rasterize_font(name, file, size, spec, mono)
        blob, index = pack_font(font, len(page1.data))
        off = page1.add(blob)
        fonts[name] = font
        font_info[name] = {'page': 1, 'offset': off, 'line_height': font['line_height'],
                           'ascent': font['ascent'], 'size': len(blob)}
        print(f'шрифт {name}: {len(blob)} байт, {len(font["glyphs"])} глифов')
    disc = calendar_disc(fonts['BODY'])            # по цифрам шрифта чисел

    # --- иконки ----------------------------------------------------------------
    emoji_path = f'{D.FONT_DIR}/{D.EMOJI_FONT}'
    big = [render_emoji(emoji_path, ch, D.ICON_BIG) for _, ch, _ in D.WEATHER_ICONS]
    small = [render_emoji(emoji_path, ch, D.ICON_SMALL) for _, ch, _ in D.WEATHER_ICONS]
    mini = [render_emoji(emoji_path, ch, D.ICON_MINI) for _, ch, _ in D.MINI_ICONS]
    icon_palette, blobs = quantize_icons(big + small + mini)
    big_blobs = blobs[:len(big)]
    small_blobs = blobs[len(big):len(big) + len(small)]
    mini_blobs = blobs[len(big) + len(small):]

    pages = [page1, Page(2), Page(GEDPL_SAFE_PAGE, hole=GEDPL_HOLE)]
    assert pages[-1].number == GEDPL_SAFE_PAGE

    def place(blob):
        for page in pages:
            if page.free_for(len(blob)):
                return page.number, page.add(blob)
        raise SystemExit('не хватает страниц данных')

    icon_tables = {'big': [], 'small': [], 'mini': []}
    for blob in small_blobs:
        icon_tables['small'].append(place(blob))
    for blob in mini_blobs:
        icon_tables['mini'].append(place(blob))
    for blob in big_blobs:
        icon_tables['big'].append(place(blob))
    # Оба круга — одним куском, чтобы попали в одну страницу (страница 1
    # почти целиком занята шрифтами).
    disc_page, disc_even = place(disc['even'] + disc['odd'])
    disc_odd = disc_even + len(disc['even'])
    for page in pages:
        print(f'страница {page.number}: занято {len(page.data)} байт')
        page.data += bytes(PAGE_SIZE - len(page.data))
        (BUILD / f'page{page.number}.bin').write_bytes(page.data)
    safe = pages[-1].data[GEDPL_HOLE[0]:GEDPL_HOLE[1]]
    assert not any(safe), 'участок, который портит GEDPL, должен быть пуст'

    palette = build_palette(icon_palette)
    cram = [D.rgb_to_cram(rgb) for rgb in palette]
    corner = make_corner(D.LAYOUT['PANEL_RADIUS'])

    # --- assets.inc --------------------------------------------------------------
    lines = ['; Сгенерировано tools/gen_assets.py — не править вручную.',
             '; Смещения внутри страниц данных; страница подключается к #0000.',
             f'ASSET_PAGES     equ {len(pages)}',
             'FONT_PAGE       equ 1',
             '; страница, у которой пуст участок #1000..#1447, портящийся при GEDPL',
             f'GEDPL_SAFE_PAGE equ {GEDPL_SAFE_PAGE}']
    for name, info in font_info.items():
        lines.append(f'FONT_{name:<10} equ #{info["offset"]:04X}')
    lines.append('; круги сегодняшнего дня: под число чётной и нечётной ширины рисунка')
    lines.append(f'DISC_PAGE       equ {disc_page}')
    lines.append(f'DISC_EVEN       equ #{disc_even:04X}')
    lines.append(f'DISC_ODD        equ #{disc_odd:04X}')
    lines.append(f'ICON_BIG_SIZE   equ {D.ICON_BIG}')
    lines.append(f'ICON_SMALL_SIZE equ {D.ICON_SMALL}')
    lines.append(f'ICON_MINI_SIZE  equ {D.ICON_MINI}')
    for kind in ('big', 'small', 'mini'):
        lines.append(f'; таблица иконок {kind}: страница, смещение (3 байта на иконку)')
        lines.append(f'IconTable_{kind}:')
        for page, off in icon_tables[kind]:
            lines.append(f'        db {page}')
            lines.append(f'        dw #{off:04X}')
    lines.append('; маска скруглённого угла панели: 0 фон, 1 рамка, 2 заливка (левый верх)')
    lines.append(f'CORNER_RADIUS   equ {D.LAYOUT["PANEL_RADIUS"]}')
    lines.append('CornerMask:')
    for row in corner:
        lines.append('        db ' + ','.join(str(v) for v in row))
    (SRC / 'assets.inc').write_text('\n'.join(lines) + '\n', encoding='utf-8')

    # --- palette.inc -------------------------------------------------------------
    lines = ['; Сгенерировано tools/gen_assets.py — палитра TS-Conf %0RRRRRGGGGGBBBBB.',
             'Palette:']
    for i in range(0, 256, 8):
        lines.append('        dw ' + ','.join(f'#{w:04X}' for w in cram[i:i + 8]))
    (SRC / 'palette.inc').write_text('\n'.join(lines) + '\n', encoding='utf-8')

    # --- strings.inc -------------------------------------------------------------
    lines = ['; Сгенерировано tools/gen_assets.py — строки CP866 (#F9 многоточие, #FA маркер).']

    def emit_string(label, text):
        lines.append(f'{label}:')
        lines.append(f'        db {asm_bytes(D.encode_cp866(text))},0 ; {text}')

    def emit_table(label, items):
        lines.append(f'{label}:')
        lines.append('        dw ' + ','.join(f'{label}_{i}' for i in range(len(items))))
        for i, text in enumerate(items):
            emit_string(f'{label}_{i}', text)

    emit_table('StrWeekdayFull', D.WEEKDAY_FULL)
    emit_table('StrWeekdayShort', D.WEEKDAY_SHORT)
    emit_table('StrMonthGen', D.MONTH_GEN)
    emit_table('StrMonthShort', D.MONTH_SHORT)
    emit_table('StrMonthUpper', D.MONTH_UPPER)
    for key, text in D.STRINGS.items():
        emit_string(f'Str_{key}', text)
    lines.append('; таблица WMO: код, иконка днём, иконка ночью, адрес описания.')
    lines.append('; Последняя запись с кодом #FF — значение по умолчанию для чужих кодов.')
    lines.append('WmoTable:')
    descs = {}
    for codes, day, night, text in D.WMO:
        label = descs.setdefault(text, f'StrWmo_{len(descs)}')
        for code in codes:
            lines.append(f'        db {code},{day},{night}')
            lines.append(f'        dw {label}')
    day, night, text = D.wmo_lookup(-1)
    lines.append(f'        db #FF,{day},{night}')
    lines.append(f'        dw {descs[text]}')
    for text, label in descs.items():
        emit_string(label, text)
    (SRC / 'strings.inc').write_text('\n'.join(lines) + '\n', encoding='utf-8')

    # --- layout.inc --------------------------------------------------------------
    lines = ['; Сгенерировано tools/gen_assets.py из design.LAYOUT.']
    for key, value in D.LAYOUT.items():
        lines.append(f'L_{key:<16} equ {value}')
    clock = clock_geometry(fonts['CLOCK'])
    lines.append('; часы: перо и прямоугольник, вычисленные по размерам шрифта')
    lines.append(f'L_CLOCK_X         equ {clock["x"]}')
    lines.append(f'L_CLOCK_Y         equ {clock["y"]}')
    for key, value in zip(('X', 'Y', 'W', 'H'), clock['box']):
        lines.append(f'L_CLOCK_BOX_{key}     equ {value}')
    lines.append('; верх круга сегодняшнего дня: центр круга — в центре цифр числа')
    lines.append(f'L_CAL_CIRCLE_Y    equ {disc["y"]}')
    lines.append(f'SCREEN_W        equ {D.SCREEN_W}')
    lines.append(f'SCREEN_H        equ {D.SCREEN_H}')
    for i, name in enumerate(D.TEXT_SET_NAMES):
        lines.append(f'TS_{name:<14} equ {i}')
    lines.append(f'PAL_BG0         equ {D.PAL_BG0}')
    lines.append(f'PAL_PANEL       equ {D.PAL_PANEL}')
    lines.append(f'PAL_BORDER      equ {D.PAL_BORDER}')
    lines.append(f'PAL_STRIP       equ {D.PAL_STRIP}')
    lines.append(f'PAL_SEP         equ {D.PAL_SEP}')
    lines.append(f'PAL_ACCENT      equ {D.PAL_ACCENT}')
    lines.append(f'PAL_TEXT0       equ {D.PAL_TEXT0}')
    (SRC / 'layout.inc').write_text('\n'.join(lines) + '\n', encoding='utf-8')

    # --- assets.json для эталона ---------------------------------------------------
    meta = {
        'fonts': font_info,
        'disc': {'page': disc_page, 'even': disc_even, 'odd': disc_odd, 'y': disc['y']},
        'icons': icon_tables,
        'icon_sizes': {'big': D.ICON_BIG, 'small': D.ICON_SMALL, 'mini': D.ICON_MINI},
        'palette_cram': cram,
        'corner': corner,
        'clock': clock,
    }
    (BUILD / 'assets.json').write_text(json.dumps(meta, indent=1), encoding='utf-8')
    print('готово:', ', '.join(f'page{p.number}.bin' for p in pages))

    if args.preview:
        import refscreen
        refscreen.preview(BUILD / 'preview.png')
        print('эталон: build/preview.png')


if __name__ == '__main__':
    main()
