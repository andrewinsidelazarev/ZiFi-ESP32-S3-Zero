"""Кодировщики команд FT812: дисплей-лист (DL) и сопроцессор.

Дисплей-лист — программа из 32-битных слов, по которой FT812 рисует каждый
кадр прямо во время развёртки. Сопроцессор принимает те же слова плюс свои
команды (CMD_*): собирает из них дисплей-лист, рисует градиенты,
распаковывает данные. Числа — по FT81X Series Programmer Guide.
Эти функции нужны генератору, эталонной сцене и тестам; в Z80 те же
команды записаны готовыми словами (src/ft812.inc).
"""
import struct

# --- форматы точек BITMAP_LAYOUT ------------------------------------------------------
ARGB1555, L1, L4, L8, RGB332, ARGB2, ARGB4, RGB565 = 0, 1, 2, 3, 4, 5, 6, 7
NEAREST, BILINEAR = 0, 1
BORDER, REPEAT = 0, 1

# --- примитивы BEGIN --------------------------------------------------------------------
BITMAPS, POINTS, LINES, LINE_STRIP, RECTS = 1, 2, 3, 4, 9

# --- смешивание и трафарет --------------------------------------------------------------
ZERO, ONE, SRC_ALPHA, DST_ALPHA, ONE_MINUS_SRC_ALPHA, ONE_MINUS_DST_ALPHA = range(6)
NEVER, LESS, LEQUAL, GREATER, GEQUAL, EQUAL, NOTEQUAL, ALWAYS = range(8)
S_ZERO, S_KEEP, S_REPLACE, S_INCR, S_DECR, S_INVERT = range(6)


def DISPLAY():
    return 0x00000000


def BITMAP_SOURCE(addr):
    return 0x01000000 | (addr & 0x3FFFFF)


def CLEAR_COLOR_RGB(r, g, b):
    return 0x02000000 | (r << 16) | (g << 8) | b


def COLOR_RGB(r, g, b):
    return 0x04000000 | (r << 16) | (g << 8) | b


def BITMAP_HANDLE(handle):
    return 0x05000000 | (handle & 0x1F)


def CELL(cell):
    return 0x06000000 | (cell & 0x7F)


def BITMAP_LAYOUT(fmt, linestride, height):
    return 0x07000000 | (fmt << 19) | ((linestride & 0x3FF) << 9) | (height & 0x1FF)


def BITMAP_SIZE(filt, wrapx, wrapy, width, height):
    return (0x08000000 | (filt << 20) | (wrapx << 19) | (wrapy << 18)
            | ((width & 0x1FF) << 9) | (height & 0x1FF))


def STENCIL_FUNC(func, ref, mask):
    return 0x0A000000 | (func << 16) | (ref << 8) | mask


def BLEND_FUNC(src, dst):
    return 0x0B000000 | (src << 3) | dst


def STENCIL_OP(sfail, spass):
    return 0x0C000000 | (sfail << 3) | spass


def POINT_SIZE(radius16):
    """Радиус точки в 1/16 пикселя."""
    return 0x0D000000 | (radius16 & 0x1FFF)


def LINE_WIDTH(width16):
    """Полуширина линии (и радиус скругления RECTS) в 1/16 пикселя."""
    return 0x0E000000 | (width16 & 0xFFF)


def COLOR_A(alpha):
    return 0x10000000 | (alpha & 0xFF)


def CLEAR_STENCIL(value):
    return 0x11000000 | (value & 0xFF)


def STENCIL_MASK(mask):
    return 0x13000000 | (mask & 0xFF)


def SCISSOR_XY(x, y):
    return 0x1B000000 | ((x & 0x7FF) << 11) | (y & 0x7FF)


def SCISSOR_SIZE(w, h):
    return 0x1C000000 | ((w & 0xFFF) << 12) | (h & 0xFFF)


def BEGIN(prim):
    return 0x1F000000 | prim


def COLOR_MASK(r, g, b, a):
    return 0x20000000 | (r << 3) | (g << 2) | (b << 1) | a


def END():
    return 0x21000000


def SAVE_CONTEXT():
    return 0x22000000


def RESTORE_CONTEXT():
    return 0x23000000


def CLEAR(color=1, stencil=1, tag=1):
    return 0x26000000 | (color << 2) | (stencil << 1) | tag


def VERTEX_FORMAT(frac):
    return 0x27000000 | (frac & 7)


def BITMAP_LAYOUT_H(linestride, height):
    return 0x28000000 | (((linestride >> 10) & 3) << 2) | ((height >> 9) & 3)


def BITMAP_SIZE_H(width, height):
    return 0x29000000 | (((width >> 9) & 3) << 2) | ((height >> 9) & 3)


def VERTEX2F(x16, y16):
    """Вершина в 1/16 пикселя (VERTEX_FORMAT 4): до 1023,9 точки по каждой оси."""
    return 0x40000000 | ((x16 & 0x7FFF) << 15) | (y16 & 0x7FFF)


# --- сопроцессор -----------------------------------------------------------------------
CMD_DLSTART = 0xFFFFFF00
CMD_SWAP = 0xFFFFFF01
CMD_GRADIENT = 0xFFFFFF0B
CMD_INFLATE = 0xFFFFFF22


def words(*values):
    return b''.join(struct.pack('<I', v & 0xFFFFFFFF) for v in values)


def cmd_gradient(x0, y0, rgb0, x1, y1, rgb1):
    return words(CMD_GRADIENT) + struct.pack('<hhIhhI', x0, y0, rgb0, x1, y1, rgb1)


def cmd_inflate(dest, compressed):
    """CMD_INFLATE: сопроцессор распаковывает поток zlib в RAM_G с адреса dest."""
    pad = (-len(compressed)) % 4
    return words(CMD_INFLATE, dest) + compressed + bytes(pad)


def rgb(color):
    r, g, b = color
    return (r << 16) | (g << 8) | b
