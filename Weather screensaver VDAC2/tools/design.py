"""Константы заставки для VDAC2 (FT812, 1024x768): цвета, шрифты, раскладка.

Строки интерфейса, таблица кодов погоды WMO и формат записи погоды — общие с
TS-Conf-версией: они берутся из ../Weather screensaver/tools/design.py, чтобы
обе заставки говорили и показывали одно и то же.
"""
import importlib.util
from pathlib import Path

_BASE_PATH = Path(__file__).resolve().parents[2] / 'Weather screensaver' / 'tools' / 'design.py'
_spec = importlib.util.spec_from_file_location('weather_base_design', _BASE_PATH)
base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(base)

# общее с TS-Conf-версией
STRINGS = base.STRINGS
WEEKDAY_FULL, WEEKDAY_SHORT = base.WEEKDAY_FULL, base.WEEKDAY_SHORT
MONTH_GEN, MONTH_SHORT, MONTH_UPPER = base.MONTH_GEN, base.MONTH_SHORT, base.MONTH_UPPER
WMO, wmo_lookup, encode_cp866 = base.WMO, base.wmo_lookup, base.encode_cp866
WEATHER_ICONS, MINI_ICONS = base.WEATHER_ICONS, base.MINI_ICONS
pack_record = base.pack_record
REC_PLACE, REC_PLACE_LEN = base.REC_PLACE, base.REC_PLACE_LEN    # название места в записи

# --- экран FT812 --------------------------------------------------------------------
SCREEN_W = 1024
SCREEN_H = 768

# --- цвета макета index.html (RGB) и прозрачности (0..255) ------------------------------
BG_TOP = (30, 41, 59)          # #1e293b — градиент от левого верхнего угла
BG_BOTTOM = (15, 23, 42)       # #0f172a
WHITE = (248, 250, 252)        # #f8fafc
MUTED = (148, 163, 184)        # #94a3b8
ACCENT = (56, 189, 248)        # #38bdf8
RAIN = (96, 165, 250)          # #60a5fa
RED = (248, 113, 113)          # #f87171 — суббота и воскресенье
DARK = BG_BOTTOM               # цифры на голубом круге
PANEL_ALPHA = 13               # панель: белый 5 % поверх фона
BORDER_ALPHA = 26              # рамка панели: белый 10 %
STRIP_ALPHA = 51               # лента недели: чёрный 20 %
SEP_ALPHA = 13                 # разделители строк прогноза: белый 5 %
GLOW_ALPHA = 60                # мягкое свечение вокруг круга сегодняшнего дня
DETAIL_ICON_ALPHA = 204        # иконки деталей чуть приглушены (0.8, как в макете)

FONT_DIR = 'C:/Windows/Fonts'
# имя: (файл, кегль в точках, набор символов). Текст рисуется со сглаживанием
# (16 уровней, формат L4): на 1024x768 это ровные края, а не «размытость»,
# как было на 360x288. Кегли — нативные, без аппаратного масштаба (он рвёт
# штрихи букв).
FONTS = {
    'CLOCK': ('segoeuil.ttf', 150, '0123456789:'),
    'TEMP': ('segoeuib.ttf', 80, '-0123456789°C'),
    'BODY': ('segoeui.ttf', 26, 'ascii+cyr+extra'),
    'SMALL': ('segoeui.ttf', 22, 'ascii+cyr+extra'),
    'CAPS': ('seguisb.ttf', 20, 'ascii+cyrupper+extra'),
    'DAY': ('seguisb.ttf', 26, '0123456789'),
}
EMOJI_FONT = 'seguiemj.ttf'
ICON_BIG = 112                 # текущая погода
ICON_SMALL = 48                # строки прогноза
ICON_MINI = 28                 # подписи деталей и булавка места

# --- раскладка (точки экрана). Y текста — базовая линия строки ------------------------------
LAYOUT = {
    'PANEL_L': (24, 24, 410, 720),
    'PANEL_R': (458, 24, 542, 720),
    'PANEL_RADIUS': 20,
    'PANEL_PAD': 28,               # внутренний отступ панелей
    # левая панель; TEXT_X — левый край текста (дата, описание, закат)
    'TEXT_X': 52,
    'LOC_ICON_X': 52, 'LOC_ICON_Y': 48, 'LOC_TEXT_X': 90, 'LOC_BASE': 72,
    'CLOCK_BASE': 222,             # часы — по центру левой панели
    'DATE1_BASE': 276, 'DATE2_BASE': 310,
    'CUR_ICON_X': 44, 'CUR_ICON_Y': 344, 'TEMP_X': 176, 'TEMP_BASE': 428,
    # «Гроза» — на одной базовой линии с «СЕНТЯБРЬ 2026» справа (как в TS-Conf)
    'DESC_BASE': 504, 'SUNSET_BASE': 540,
    'DETAIL_ICON_X': 52, 'DETAIL_TEXT_X': 94, 'DETAIL_Y0': 580, 'DETAIL_STEP': 44,
    'DETAIL_DBASE': 21,            # базовая линия текста детали от верха иконки
    # правая панель
    'FC_X': 490, 'FC_RIGHT': 968, 'FC_TITLE_BASE': 72,
    'FC_ROW_Y0': 92, 'FC_ROW_STEP': 72, 'FC_ROWS': 5,
    'FC_DAY_DBASE': 44, 'FC_ICON_X': 700, 'FC_ICON_DY': 12,
    'FC_TEMP_DBASE': 32, 'FC_RAIN_DBASE': 60, 'FC_SEP_DY': 71,
    'CAL_TITLE_BASE': 504,
    'STRIP': (486, 524, 486, 192), 'STRIP_RADIUS': 16,
    'CAL_COL_X0': 490, 'CAL_COL_W': 68,
    'CAL_LABEL_BASE': 574, 'CAL_NUM_BASE': 648, 'CAL_CIRCLE_D': 56,
    'CAPS_SPACING': 2,             # заголовки — вразрядку, как в макете
}
