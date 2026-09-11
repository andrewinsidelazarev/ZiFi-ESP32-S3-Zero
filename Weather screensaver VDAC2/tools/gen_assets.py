"""Ресурсы заставки для FT812: шрифты, иконки и их раскладка в RAM_G.

Шрифты. Каждый нужный символ растрируется в свою ячейку одинакового размера
(формат L4: 16 уровней яркости, две точки в байте, левая — в старшей
тетраде). Ячейки идут подряд, и одна «ручка» (bitmap handle) FT812 охватывает
до 128 ячеек: символ рисуется командами CELL(n) + VERTEX2F. Для каждого
шрифта генератор выдаёт таблицы: код CP866 -> номер ячейки, шаг символа
(advance) и края видимых точек (для точного центрирования).

Иконки. Эмодзи Segoe UI Emoji на прозрачном фоне в формате ARGB4 (по 4 бита
на прозрачность и цвет): панели макета полупрозрачные, и иконки ложатся на
них без ореола.

Всё это складывается в образ памяти RAM_G (build/ramg.bin) и его сжатую
копию (build/ramg.z): её распаковывает сам FT812 командой CMD_INFLATE.
Раскладка — в build/assets.json (для эталона и тестов).

Запуск: python tools/gen_assets.py [--preview]
"""
import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import design as D  # noqa: E402
import eve  # noqa: E402

HERE = Path(__file__).resolve().parent
PROJ = HERE.parent
BUILD = PROJ / 'build'
SRC = PROJ / 'src'
RAM_G_SIZE = 1024 * 1024
CELLS_PER_HANDLE = 128
FIRST_HANDLE = 0               # ручка 15 — рабочая у сопроцессора, её не трогаем


def charset(spec):
    """Коды CP866 для набора символов шрифта."""
    if spec == 'ascii+cyr+extra':
        # #F2..#F7 — Є є Ї ї Ў ў: названия мест Украины и Беларуси
        # («Київ»); І і прошивка присылает латинскими I i
        codes = (list(range(0x20, 0x7F)) + list(range(0x80, 0xB0)) +
                 list(range(0xE0, 0xF8)) + [0xF8, 0xF9, 0xFA])
    elif spec == 'ascii+cyrupper+extra':
        # заголовки и подписи дней — только прописные: цифры, латиница A-Z,
        # знаки и кириллица А-Я, Ё — укладывается в одну ручку (128 ячеек)
        codes = list(range(0x20, 0x60)) + list(range(0x80, 0xA0)) + [0xF0, 0xF8]
    else:
        codes = [D.encode_cp866(ch)[0] for ch in spec]
    return sorted(set(codes))


def decode_code(code):
    if code == 0xF9:
        return '…'
    if code == 0xFA:
        return '•'
    return bytes([code]).decode('cp866')


def rasterize_font(name, file, size, spec):
    """Шрифт -> ячейки L4 и таблицы. Возвращает словарь с данными шрифта."""
    font = ImageFont.truetype(f'{D.FONT_DIR}/{file}', size)
    ascent, descent = font.getmetrics()
    codes = charset(spec)
    boxes = {}
    for code in codes:
        ch = decode_code(code)
        boxes[code] = (int(round(font.getlength(ch))), font.getbbox(ch, anchor='ls'))
    # ячейка вмещает любой символ: слева запас под отрицательный отступ,
    # сверху — под выносные элементы выше ascent (Й, Ё)
    pad = max(0, -min(b[0] for _, b in boxes.values()))
    top = max(ascent, -min(b[1] for _, b in boxes.values()))
    bottom = max(descent, max(b[3] for _, b in boxes.values()))
    width = max(b[2] for _, b in boxes.values()) + pad
    width += width & 1                                   # L4: две точки в байте
    height = top + bottom
    cells = []
    table = {}
    for code in codes:
        adv, (x0, y0, x1, y1) = boxes[code]
        img = Image.new('L', (width, height), 0)
        draw = ImageDraw.Draw(img)
        draw.text((pad, top), decode_code(code), font=font, fill=255, anchor='ls')
        ink = img.point(lambda v: 255 if v >= 8 else 0).getbbox()
        # края видимых точек относительно пера и базовой линии
        left, right = (ink[0] - pad, ink[2] - pad) if ink else (0, 0)
        ink_top, ink_bottom = (ink[1] - top, ink[3] - top) if ink else (0, 0)
        table[code] = {'cell': len(cells), 'adv': adv, 'left': left, 'right': right,
                       'top': ink_top, 'bottom': ink_bottom, 'dx': 0, 'dy': 0}
        cells.append(pack_l4(img))
    f = {'name': name, 'size': size, 'ascent': ascent, 'descent': descent,
         'cell_w': width, 'cell_h': height, 'pad': pad, 'top': top,
         'stride': width // 2, 'format': eve.L4, 'cells': cells, 'table': table}
    if name == 'CLOCK':
        clock_digits(f)
    return f


def clock_digits(f):
    """Часы: все цифры одной ширины (иначе «11:11» уже «00:00», и часы по
    центру панели прыгали бы при смене минут), рисунок каждой — посередине
    своей клетки; двоеточие — по вертикальному центру цифр (в Segoe UI оно
    стоит на высоте строчных букв, ниже середины цифр). Сдвиги рисунка
    относительно пера — поля dx и dy таблицы."""
    t = f['table']
    digits = [ord(c) for c in '0123456789']
    cell = max(t[c]['adv'] for c in digits)
    for c in digits:
        g = t[c]
        ink_w = g['right'] - g['left']
        left = (cell - ink_w) // 2
        g['dx'] = left - g['left']
        g['left'], g['right'], g['adv'] = left, left + ink_w, cell
    colon = t[ord(':')]
    digit_mid2 = min(t[c]['top'] for c in digits) + max(t[c]['bottom'] for c in digits)
    colon_mid2 = colon['top'] + colon['bottom']
    colon['dy'] = (digit_mid2 - colon_mid2) // 2        # удвоенные центры: без дробей
    colon['top'] += colon['dy']
    colon['bottom'] += colon['dy']


def pack_l4(img):
    """'L' 0..255 -> L4: 16 уровней, левая точка в старшей тетраде."""
    w, h = img.size
    px = img.load()
    out = bytearray()
    for y in range(h):
        for x in range(0, w, 2):
            a = (px[x, y] * 15 + 127) // 255
            b = (px[x + 1, y] * 15 + 127) // 255
            out.append((a << 4) | b)
    return bytes(out)


def render_emoji(ch, box):
    """Эмодзи на прозрачном фоне, вписанный в квадрат box x box (RGBA)."""
    font = ImageFont.truetype(f'{D.FONT_DIR}/{D.EMOJI_FONT}', 256)
    canvas = Image.new('RGBA', (400, 400), (0, 0, 0, 0))
    ImageDraw.Draw(canvas).text((60, 40), ch, font=font, embedded_color=True)
    bbox = canvas.getchannel('A').getbbox()
    assert bbox, f'эмодзи {ch!r} не отрисовался'
    content = canvas.crop(bbox)
    inner = box - 2
    scale = min(inner / content.width, inner / content.height)
    nw = max(1, round(content.width * scale))
    nh = max(1, round(content.height * scale))
    # уменьшать в «предумноженной» альфе (RGBa): иначе по краям — тёмный ореол
    small = content.convert('RGBa').resize((nw, nh), Image.LANCZOS).convert('RGBA')
    out = Image.new('RGBA', (box, box), (0, 0, 0, 0))
    out.paste(small, ((box - nw) // 2, (box - nh) // 2))
    return out


def pack_argb4(img):
    """RGBA -> ARGB4 (16 бит на точку, младшим байтом вперёд)."""
    raw = img.tobytes()                                  # R, G, B, A подряд
    out = bytearray()
    for i in range(0, len(raw), 4):
        r, g, b, a = raw[i:i + 4]
        word = ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)
        out += word.to_bytes(2, 'little')
    return bytes(out)


def font_tables(meta):
    """Таблицы шрифтов для Z80: у каждого шрифта 256 байт «код CP866 ->
    номер ячейки» (#FF — символа нет) и по 8 байт на ячейку: шаг пера,
    сдвиги рисунка dx и dy, левый и правый край видимых точек, верх и низ
    видимых точек от базовой линии, признак «есть точки» (у пробела нет).
    Вернуть (байты, {шрифт: (смещение карты, смещение ячеек)})."""
    blob = bytearray()
    offsets = {}
    for name, f in meta['fonts'].items():
        cells = sorted(f['table'].items(), key=lambda kv: kv[1]['cell'])
        fmap = bytearray([0xFF]) * 256
        records = bytearray()
        for code, g in cells:
            assert g['cell'] < 0xFF
            fmap[int(code)] = g['cell']
            ink = 1 if g['right'] > g['left'] else 0
            records += struct.pack('<BbbbBbbB', g['adv'], g['dx'], g['dy'], g['left'],
                                   g['right'], g['top'], g['bottom'], ink)
        offsets[name] = (len(blob), len(blob) + 256)
        blob += fmap + records
    return bytes(blob), offsets


# Страницы данных плагина: четыре по 16 КиБ после страницы кода. GEDPL
# (MNGV_PL с нулём при выходе) пишет палитру WC через окно FMADDR на #1000 и
# портит #1000..#1447 страницы, подключённой к #0000; перед выходом плагин
# подключает страницу GEDPL_SAFE_PAGE, у которой этот участок пуст.
PAGE_SIZE = 16384
DATA_PAGES = 4
GEDPL_HOLE = (0x1000, 0x1448)
GEDPL_SAFE_PAGE = DATA_PAGES


class Page:
    """Страница данных 16 КиБ; ресурсы кладутся подряд, минуя дыру hole."""

    def __init__(self, number, hole=None):
        self.number = number
        self.data = bytearray()
        self.hole = hole

    def add(self, blob):
        start = len(self.data)
        if self.hole and start < self.hole[1] and start + len(blob) > self.hole[0]:
            start = self.hole[1]
        assert start + len(blob) <= PAGE_SIZE, f'страница {self.number} переполнена'
        self.data += bytes(start - len(self.data)) + blob
        return start


def place_stream(pages, data):
    """Разложить поток по страницам подряд, обходя дыру. Вернуть отрезки
    (страница, адрес в окне #0000, длина) — по ним Z80 передаёт поток чипу."""
    segments = []
    pos = 0
    for page in pages:
        while pos < len(data) and len(page.data) < PAGE_SIZE:
            start = len(page.data)
            if page.hole and page.hole[0] <= start < page.hole[1]:
                page.data += bytes(page.hole[1] - start)
                continue
            limit = page.hole[0] if page.hole and start < page.hole[0] else PAGE_SIZE
            count = min(limit - start, len(data) - pos)
            page.data += data[pos:pos + count]
            segments.append((page.number, start, count))
            pos += count
    assert pos == len(data), 'сжатые ресурсы не влезают в страницы плагина'
    return segments


def dl_hex(word):
    return f'#{word & 0xFFFFFFFF:08X}'


def write_assets_inc(meta, table_offsets, tables_addr, static_addr, static_size, segments,
                     packed_size):
    lines = ['; Сгенерировано tools/gen_assets.py — не править вручную.',
             '; Страницы данных подключаются к окну #0000 (API WC 78, MNG0_PL).',
             f'DATA_PAGES      equ {DATA_PAGES}',
             f'GEDPL_SAFE_PAGE equ {GEDPL_SAFE_PAGE}  ; участок #1000..#1447 пуст',
             'TABLES_PAGE     equ 1                ; таблицы шрифтов и неизменный блок',
             f'STATIC_ADDR     equ #{static_addr:04X}',
             f'STATIC_SIZE     equ {static_size}',
             f'ASSET_PACKED    equ {packed_size}          ; сжатый zlib образ RAM_G',
             f'ASSET_PAD       equ {(-packed_size) % 4}                ; добивка до 4 байт',
             f'ICON_BIG_HANDLE   equ {meta["icons"]["big"]["handle"]}',
             f'ICON_SMALL_HANDLE equ {meta["icons"]["small"]["handle"]}',
             f'ICON_MINI_HANDLE  equ {meta["icons"]["mini"]["handle"]}',
             '',
             '; Шрифты: отступ ячейки слева (pad), базовая линия в ячейке (top),',
             '; ручки ячеек 0..127 и 128..255, адреса карты и ячеек в TABLES_PAGE.']
    for name, f in meta['fonts'].items():
        handles = [h['handle'] for h in f['handles']]
        h1 = handles[1] if len(handles) > 1 else handles[0]
        fmap, cells = table_offsets[name]
        lines += [f'Font_{name}:',
                  f'        db {f["pad"]},{f["top"]},{handles[0]},{h1}',
                  f'        dw #{tables_addr + fmap:04X},#{tables_addr + cells:04X}']
    lines += ['', '; Сжатый образ RAM_G по страницам: страница, адрес в окне, длина.',
              'AssetSegments:']
    for page, addr, count in segments:
        lines += [f'        db {page}', f'        dw #{addr:04X},{count}']
    lines += ['        db #FF']
    (SRC / 'assets.inc').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def write_layout_inc(assets):
    """Координаты и готовые слова дисплей-листа для src/screen.asm."""
    import refscene
    L = D.LAYOUT
    lines = ['; Сгенерировано tools/gen_assets.py из design.py — не править вручную.',
             f'SCREEN_W        equ {D.SCREEN_W}',
             f'SCREEN_H        equ {D.SCREEN_H}']
    for key, value in L.items():
        if isinstance(value, tuple):
            for suffix, v in zip(('X', 'Y', 'W', 'H'), value):
                lines.append(f'L_{key}_{suffix:<10} equ {v}')
        else:
            lines.append(f'L_{key:<16} equ {value}')
    clock = assets.glyph(assets.font('CLOCK'), ord(':'))
    lines += ['; вычислено по шрифтам',
              f'L_CLOCK_X          equ {refscene.clock_x(assets)}',
              f'L_COLON_ADV        equ {clock["adv"]}',
              f'L_LEFT_TEXT_RIGHT  equ {refscene.left_text_right()}',
              f'L_CIRCLE_R16       equ {L["CAL_CIRCLE_D"] * 8}',
              '; прозрачности (0..255)',
              f'A_SEP           equ {D.SEP_ALPHA}',
              f'A_GLOW          equ {D.GLOW_ALPHA}',
              f'A_DETAIL_ICON   equ {D.DETAIL_ICON_ALPHA}',
              '; готовые слова дисплей-листа: цвета COLOR_RGB',
              f'DL_WHITE        equ {dl_hex(eve.COLOR_RGB(*D.WHITE))}',
              f'DL_MUTED        equ {dl_hex(eve.COLOR_RGB(*D.MUTED))}',
              f'DL_ACCENT       equ {dl_hex(eve.COLOR_RGB(*D.ACCENT))}',
              f'DL_RAIN         equ {dl_hex(eve.COLOR_RGB(*D.RAIN))}',
              f'DL_DARK         equ {dl_hex(eve.COLOR_RGB(*D.DARK))}',
              f'DL_ICON         equ {dl_hex(eve.COLOR_RGB(255, 255, 255))}']
    (SRC / 'layout.inc').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def write_strings_inc():
    """Строки CP866 и таблица WMO — в том же виде, что у TS-Conf-версии
    (их читают общие ../shared/weather/weather.asm и saver.asm)."""
    lines = ['; Сгенерировано tools/gen_assets.py — строки CP866 (#F9 многоточие, #FA маркер).']

    def emit_string(label, text):
        data = ','.join(f'#{b:02X}' for b in D.encode_cp866(text))
        lines.append(f'{label}:')
        lines.append(f'        db {data},0 ; {text}')

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


class RamG:
    """Раскладка ресурсов в RAM_G подряд с выравниванием на 4 байта."""

    def __init__(self):
        self.data = bytearray()

    def add(self, blob):
        self.data += bytes((-len(self.data)) % 4)
        addr = len(self.data)
        self.data += blob
        assert len(self.data) <= RAM_G_SIZE, 'ресурсы не влезают в RAM_G'
        return addr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--preview', action='store_true',
                        help='после генерации нарисовать build/preview.png на FT812')
    args = parser.parse_args()
    BUILD.mkdir(exist_ok=True)
    ram = RamG()
    handle = FIRST_HANDLE
    meta = {'fonts': {}, 'icons': {}}

    for name, (file, size, spec) in D.FONTS.items():
        f = rasterize_font(name, file, size, spec)
        cell_bytes = f['stride'] * f['cell_h']
        handles = []
        for start in range(0, len(f['cells']), CELLS_PER_HANDLE):
            chunk = f['cells'][start:start + CELLS_PER_HANDLE]
            handles.append({'handle': handle, 'source': ram.add(b''.join(chunk))})
            handle += 1
        meta['fonts'][name] = {
            'handles': handles, 'format': f['format'], 'stride': f['stride'],
            'cell_w': f['cell_w'], 'cell_h': f['cell_h'], 'pad': f['pad'], 'top': f['top'],
            'ascent': f['ascent'], 'descent': f['descent'], 'size': f['size'],
            'table': {str(code): t for code, t in f['table'].items()},
        }
        print(f'шрифт {name}: {len(f["cells"])} символов, ячейка {f["cell_w"]}x{f["cell_h"]}, '
              f'{len(f["cells"]) * cell_bytes} байт, ручки {[h["handle"] for h in handles]}')

    for kind, size, icons in (('big', D.ICON_BIG, D.WEATHER_ICONS),
                              ('small', D.ICON_SMALL, D.WEATHER_ICONS),
                              ('mini', D.ICON_MINI, D.MINI_ICONS)):
        blob = b''.join(pack_argb4(render_emoji(ch, size)) for _, ch, _ in icons)
        meta['icons'][kind] = {'handle': handle, 'source': ram.add(blob), 'size': size,
                               'format': eve.ARGB4, 'stride': size * 2, 'count': len(icons)}
        print(f'иконки {kind}: {len(icons)} x {size}px, {len(blob)} байт, ручка {handle}')
        handle += 1
    assert handle <= 15, 'ручек FT812 не хватает'

    raw = bytes(ram.data)
    packed = zlib.compress(raw, 9)
    (BUILD / 'ramg.bin').write_bytes(raw)
    (BUILD / 'ramg.z').write_bytes(packed)
    meta['ramg_size'] = len(raw)
    meta['packed_size'] = len(packed)
    (BUILD / 'assets.json').write_text(json.dumps(meta, indent=1), encoding='utf-8')
    print(f'RAM_G: {len(raw)} байт, сжато zlib: {len(packed)} байт')

    # --- для Z80: таблицы шрифтов, неизменный блок кадра, страницы данных ------------------
    import refscene
    assets = refscene.Assets()
    tables, table_offsets = font_tables(meta)
    static = refscene.static_part(assets)
    pages = [Page(n, GEDPL_HOLE if n == GEDPL_SAFE_PAGE else None)
             for n in range(1, DATA_PAGES + 1)]
    tables_addr = pages[0].add(tables)
    static_addr = pages[0].add(static)
    segments = place_stream(pages, packed)
    for page in pages:
        print(f'страница {page.number}: занято {len(page.data)} байт')
        page.data += bytes(PAGE_SIZE - len(page.data))
        (BUILD / f'page{page.number}.bin').write_bytes(page.data)
    safe = pages[-1].data[GEDPL_HOLE[0]:GEDPL_HOLE[1]]
    assert not any(safe), 'участок, который портит GEDPL, должен быть пуст'
    print(f'неизменный блок кадра: {len(static)} байт; таблицы шрифтов: {len(tables)} байт')

    write_assets_inc(meta, table_offsets, tables_addr, static_addr, len(static), segments,
                     len(packed))
    write_layout_inc(assets)
    write_strings_inc()
    meta['static'] = {'page': 1, 'addr': static_addr, 'size': len(static)}
    meta['segments'] = segments
    (BUILD / 'assets.json').write_text(json.dumps(meta, indent=1), encoding='utf-8')

    if args.preview:
        import refscene
        refscene.preview(BUILD / 'preview.png')
        print('кадр FT812: build/preview.png')


if __name__ == '__main__':
    main()
