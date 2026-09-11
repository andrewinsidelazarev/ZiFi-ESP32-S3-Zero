"""Единый источник констант заставки: цвета, раскладка экрана, строки.

Эти же величины попадают в ассемблер через генерируемые файлы src/layout.inc
и src/strings.inc, поэтому Python-эталон (refscreen.py) и код Z80 рисуют по
одним и тем же числам.
"""

# --- Экран TS-Conf 360x288, режим 256 цветов --------------------------------
SCREEN_W = 360
SCREEN_H = 288

# --- Цвета макета index.html (RGB) ------------------------------------------
BG_TOP = (30, 41, 59)          # #1e293b — верх радиального градиента
BG_BOTTOM = (15, 23, 42)       # #0f172a — низ
PANEL = (28, 38, 60)           # фон панели (5 % белого поверх фона)
BORDER = (52, 62, 84)          # рамка панели (10 % белого)
STRIP = (16, 24, 40)           # фон ленты календаря (20 % чёрного)
SEP = (38, 48, 68)             # разделитель строк прогноза
ACCENT = (56, 189, 248)        # #38bdf8
WHITE = (248, 250, 252)        # #f8fafc
MUTED = (148, 163, 184)        # #94a3b8
RAIN = (96, 165, 250)          # #60a5fa
RED = (248, 113, 113)          # #f87171 — суббота и воскресенье в календаре
DARK = BG_BOTTOM               # текст на голубом кружке активного дня

# --- Палитра: фиксированные индексы --------------------------------------------
PAL_BG0 = 0                    # 0..15 — градиент фона сверху вниз
PAL_BG_COUNT = 16
PAL_PANEL = 16
PAL_BORDER = 17
PAL_STRIP = 18
PAL_SEP = 19
PAL_ACCENT = 20
PAL_WHITE = 21
PAL_MUTED = 22
PAL_DARK = 23
PAL_TEXT0 = 24                 # наборы текста, по 3 уровня (24..50)
PAL_ICONS = 51                 # квантованные цвета иконок
PAL_ICON_COLORS = 128          # 51..178

# Наборы текста: (цвет чернил, цвет подложки). Индекс набора = позиция в списке.
TEXT_SETS = [
    ('white_panel', WHITE, PANEL),
    ('muted_panel', MUTED, PANEL),
    ('accent_panel', ACCENT, PANEL),
    ('rain_panel', RAIN, PANEL),
    ('white_strip', WHITE, STRIP),
    ('muted_strip', MUTED, STRIP),
    ('dark_accent', DARK, ACCENT),
    ('accent_strip', ACCENT, STRIP),
    ('red_strip', RED, STRIP),
]
TEXT_SET_INDEX = {name: i for i, (name, _, _) in enumerate(TEXT_SETS)}

# --- Шрифты (Windows) ------------------------------------------------------------
FONT_DIR = 'C:/Windows/Fonts'
FONTS = {
    # имя: (файл, кегль, набор символов, одноцветный)
    # Все шрифты растрируются без сглаживания, по хинтингу шрифта: на экране
    # 360x288 полутона сглаживания выглядели размыто (проверено на Evo).
    # Для мелкого текста — Tahoma: она рисовалась под экранные точки и чётко
    # читается в 11-13 точек, в том числе кириллица.
    # Часы: крупно, на всю ширину левой панели (цифры моноширинные, см.
    # gen_assets.tabular_digits), по горизонтали — по центру панели.
    'CLOCK': ('segoeui.ttf', 58, '0123456789:', True),
    'TEMP': ('segoeuib.ttf', 28, '0123456789-°C', True),
    'BODY': ('tahoma.ttf', 13, 'ascii+cyr+extra', True),
    'CAPS': ('tahomabd.ttf', 11, 'ascii+cyrupper+extra', True),
}
EMOJI_FONT = 'seguiemj.ttf'

# --- Иконки ---------------------------------------------------------------------
# Большие (48 px) — текущая погода; малые (24 px) — строки прогноза;
# мини (16 px) — подписи деталей. Ключ — символ эмодзи без VS16.
ICON_BIG = 48
ICON_SMALL = 24
ICON_MINI = 16
WEATHER_ICONS = [
    # id, эмодзи, комментарий
    (0, '\u2600', 'солнце — ясно днём'),
    (1, '\U0001f319', 'луна — ясно ночью'),
    (2, '\u26c5', 'солнце за облаком — переменная облачность'),
    (3, '\U0001f325', 'большое облако с солнцем — облачно'),
    (4, '\u2601', 'облако — пасмурно'),
    (5, '\U0001f32b', 'туман'),
    (6, '\U0001f326', 'морось'),
    (7, '\U0001f327', 'дождь'),
    (8, '\U0001f328', 'снег'),
    (9, '\u26c8', 'гроза'),
]
MINI_ICONS = [
    (0, '\U0001f4a7', 'капля — осадки'),
    (1, '\U0001f4a8', 'ветер'),
    (2, '\U0001f9ed', 'компас — давление'),
    (3, '\U0001f4cd', 'булавка — место'),
    (4, '\U0001f305', 'закат'),
]

# Код погоды WMO -> (иконка днём, иконка ночью, описание).
WMO = [
    ((0,), 0, 1, 'Ясно'),
    ((1,), 0, 1, 'Малооблачно'),
    ((2,), 2, 4, 'Облачно'),           # ночью — облако без солнца
    ((3,), 4, 4, 'Пасмурно'),
    ((45, 48), 5, 5, 'Туман'),
    ((51, 53, 55, 56, 57), 6, 6, 'Морось'),
    ((61, 80), 7, 7, 'Дождь'),
    ((63, 81), 7, 7, 'Дождь'),
    ((65, 82), 7, 7, 'Сильный дождь'),
    ((66, 67), 7, 7, 'Ледяной дождь'),
    ((71, 73, 77, 85), 8, 8, 'Снег'),
    ((75, 86), 8, 8, 'Сильный снег'),
    ((95,), 9, 9, 'Гроза'),
    ((96, 99), 9, 9, 'Гроза с градом'),
]


def wmo_lookup(code):
    """Вернуть (иконка днём, иконка ночью, описание) для кода WMO."""
    for codes, day, night, text in WMO:
        if code in codes:
            return day, night, text
    return 4, 4, 'Облачно'


# --- Раскладка экрана (пиксели) ------------------------------------------------
LAYOUT = {
    'PANEL_L_X': 8, 'PANEL_L_Y': 8, 'PANEL_L_W': 148, 'PANEL_L_H': 272,
    'PANEL_R_X': 162, 'PANEL_R_Y': 8, 'PANEL_R_W': 190, 'PANEL_R_H': 272,
    'PANEL_RADIUS': 6,
    # левая панель (внутренняя область 18..145)
    'LOC_ICON_X': 16, 'LOC_ICON_Y': 18, 'LOC_TEXT_X': 35, 'LOC_TEXT_Y': 18,
    # верх цифр часов; X и Y пера часов вычисляет gen_assets по размерам шрифта
    'CLOCK_DIGIT_TOP': 44,
    'DATE1_X': 18, 'DATE1_Y': 96,
    'DATE2_X': 18, 'DATE2_Y': 112,
    'CUR_ICON_X': 14, 'CUR_ICON_Y': 134,
    'TEMP_X': 66, 'TEMP_Y': 140,
    # Описание погоды («Гроза») стоит на одной базовой линии с заголовком
    # календаря «СЕНТЯБРЬ 2026» в правой панели: DESC_Y + ascent BODY =
    # CAL_TITLE_Y + ascent CAPS (проверяет тест). Закат — следующей строкой,
    # детали — сразу за ним: низ последней строки в 7 точках от рамки.
    'DESC_X': 18, 'DESC_Y': 188,
    'SUNSET_X': 18, 'SUNSET_Y': 204,
    'DETAIL_ICON_X': 18, 'DETAIL_TEXT_X': 39,
    'DETAIL_Y0': 222, 'DETAIL_STEP': 17,
    # правая панель (внутренняя область 172..341): строка прогноза в две
    # линии — слева день и иконка, справа температура, под ней осадки
    'FC_TITLE_X': 172, 'FC_TITLE_Y': 20,
    'FC_ROW_Y0': 36, 'FC_ROW_STEP': 28, 'FC_ROWS': 5,
    'FC_DAY_X': 172, 'FC_DAY_DY': 4,
    'FC_ICON_X': 250, 'FC_ICON_DY': 1,
    'FC_RIGHT': 341, 'FC_TEMP_DY': -1, 'FC_RAIN_DY': 14,
    'FC_SEP_X0': 172, 'FC_SEP_X1': 341, 'FC_SEP_DY': 26,
    # Календарь опущен к низу панели: лента не жмётся к заголовку месяца
    # (зазор 9 точек), а под ней до рамки остаётся 5 — как у текста слева.
    'CAL_TITLE_X': 172, 'CAL_TITLE_Y': 190,
    'STRIP_X': 172, 'STRIP_Y': 210, 'STRIP_W': 170, 'STRIP_H': 64,
    'CAL_COL_X0': 173, 'CAL_COL_W': 24,
    # Круг сегодняшнего дня ставит gen_assets.calendar_disc: его центр — ровно
    # в центре цифр числа (по метрикам шрифта BODY), верх — L_CAL_CIRCLE_Y.
    # Диаметр нечётный, как и высота цифр (9 точек): так круг по вертикали
    # встаёт по точкам ровно, без полуточки.
    'CAL_LABEL_Y': 218, 'CAL_NUM_Y': 242,
    'CAL_CIRCLE_D': 23,
}

# --- Строки интерфейса (кодируются в CP866 генератором) ------------------------
WEEKDAY_FULL = ['Понедельник', 'Вторник', 'Среда', 'Четверг', 'Пятница',
                'Суббота', 'Воскресенье']
WEEKDAY_SHORT = ['ПН', 'ВТ', 'СР', 'ЧТ', 'ПТ', 'СБ', 'ВС']
MONTH_GEN = ['января', 'февраля', 'марта', 'апреля', 'мая', 'июня', 'июля',
             'августа', 'сентября', 'октября', 'ноября', 'декабря']
MONTH_SHORT = ['янв', 'фев', 'мар', 'апр', 'мая', 'июн', 'июл', 'авг',
               'сен', 'окт', 'ноя', 'дек']
MONTH_UPPER = ['ЯНВАРЬ', 'ФЕВРАЛЬ', 'МАРТ', 'АПРЕЛЬ', 'МАЙ', 'ИЮНЬ', 'ИЮЛЬ',
               'АВГУСТ', 'СЕНТЯБРЬ', 'ОКТЯБРЬ', 'НОЯБРЬ', 'ДЕКАБРЬ']
STRINGS = {
    'TITLE_FORECAST': 'ПРОГНОЗ НА 5 ДНЕЙ',
    'SUNSET_PREFIX': 'Закат в ',
    'UNIT_PRECIP': ' мм осадков',
    'UNIT_WIND': ' км/ч (ветер)',
    'UNIT_PRESSURE': ' мм рт. ст.',
    'UNIT_MM': ' мм',
    'UNIT_MM_UPPER': ' ММ',
    'DEG_C': '\u00b0C',
    'DEG': '\u00b0',
    'DOTS': '\u2026',
    'NO_ZIFI': 'ZiFi не найден',
    'NO_INI': 'Нет /zifi/zifi.ini',
    'INI_TOO_LONG': 'zifi.ini больше 1 КБ',
    'NO_WIFI': 'Нет связи Wi-Fi',
    'NO_WEATHER': 'Нет данных о погоде',
    'LOADING': 'Запрос погоды\u2026',
}

# Извлечённые из TEXT_SETS имена для asm
TEXT_SET_NAMES = [name.upper() for name, _, _ in TEXT_SETS]

# --- Запись погоды: ответ прошивки на WEATHER_GET (все числа little-endian) ----
# Смещения одинаковы в прошивке (weather_service.hpp), в asm (weather.asm)
# и здесь; менять только вместе с REC_VERSION.
REC_VERSION = 1
REC_STATUS = 0          # 1 — данные есть, 0 — ошибка (текст в докладе #EE)
REC_VER = 1             # версия формата
REC_PLACE = 2           # название места, CP866, ноль в конце, 24 байта
REC_PLACE_LEN = 24
REC_TEMP = 26           # текущая температура, °C, знаковый байт
REC_CODE = 27           # код погоды WMO
REC_IS_DAY = 28         # 1 — день (иконка солнца), 0 — ночь
REC_WIND10 = 29         # ветер, км/ч x10, u16
REC_PRECIP10 = 31       # осадки за час, мм x10, u16
REC_PRESS = 33          # давление у поверхности, мм рт. ст., u16
REC_SUNRISE = 35        # восход: час, минута
REC_SUNSET = 37         # закат: час, минута
REC_DATA_TIME = 39      # местное время данных: час, минута
REC_DAY_COUNT = 41      # число дневных записей (6: сегодня + 5)
REC_DAYS = 42           # дневные записи по 8 байт
DAY_SIZE = 8
DAY_DAY = 0             # число месяца
DAY_MONTH = 1           # месяц 1..12
DAY_WDAY = 2            # день недели 0=понедельник
DAY_CODE = 3            # код WMO
DAY_TMIN = 4            # минимум, °C, знаковый
DAY_TMAX = 5            # максимум, °C, знаковый
DAY_PRECIP10 = 6        # осадки за сутки, мм x10, u16
REC_MAX_DAYS = 6
REC_SIZE = REC_DAYS + DAY_SIZE * REC_MAX_DAYS   # 90


def pack_record(place, temp, code, is_day, wind10, precip10, press,
                sunrise, sunset, data_time, days):
    """Собрать запись погоды (для тестов и модели ESP)."""
    import struct
    rec = bytearray(REC_SIZE)
    rec[REC_STATUS] = 1
    rec[REC_VER] = REC_VERSION
    name = encode_cp866(place)[:REC_PLACE_LEN - 1]
    rec[REC_PLACE:REC_PLACE + len(name)] = name
    struct.pack_into('<bBBHHHBBBBBBB', rec, REC_TEMP, temp, code, is_day, wind10,
                     precip10, press, sunrise[0], sunrise[1], sunset[0], sunset[1],
                     data_time[0], data_time[1], len(days))
    for i, (day, month, wday, wcode, tmin, tmax, dprecip10) in enumerate(days):
        struct.pack_into('<BBBBbbH', rec, REC_DAYS + i * DAY_SIZE, day, month, wday,
                         wcode, tmin, tmax, dprecip10)
    return bytes(rec)


# Замены знаков, которых нет в CP866, — те же, что делает прошивка
# (utf8ToCp866 в src/weather_parse.cpp): украинские І і похожи на латинские
# I i, Ґ ґ -> Г г; неразрывный пробел, типографские апострофы, тире и
# кавычки -> знаки ASCII. Є є Ї ї Ў ў в CP866 есть (#F2..#F7).
CP866_SUBSTITUTES = str.maketrans({
    '\u0406': 'I', '\u0456': 'i',                  # І і
    '\u0490': '\u0413', '\u0491': '\u0433',        # Ґ ґ -> Г г
    '\u00a0': ' ',                                 # неразрывный пробел
    '\u02bc': "'", '\u2018': "'", '\u2019': "'",   # апострофы
    '\u2013': '-', '\u2014': '-',                  # тире
    '\u00ab': '"', '\u00bb': '"', '\u201c': '"', '\u201d': '"', '\u201e': '"'})


def encode_cp866(text):
    """Строка Python -> байты CP866 с двумя нестандартными знаками:
    U+2026 (многоточие) -> #F9, U+2022 (маркер) -> #FA. Знаки, которых
    в CP866 нет, заменяются как в прошивке (CP866_SUBSTITUTES)."""
    out = bytearray()
    for ch in text.translate(CP866_SUBSTITUTES):
        if ch == '\u2026':
            out.append(0xF9)
        elif ch == '\u2022':
            out.append(0xFA)
        else:
            out += ch.encode('cp866')
    return bytes(out)


def blend(ink, paper, level):
    """Смешать чернила с подложкой: level 1..3 из 3 (сглаживание)."""
    k = level / 3.0
    return tuple(int(round(paper[i] + (ink[i] - paper[i]) * k)) for i in range(3))


def rgb_to_cram(rgb):
    """RGB888 -> слово палитры TS-Conf %0RRRRRGGGGGBBBBB."""
    r, g, b = rgb
    return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)


def cram_to_rgb(word):
    """Обратное преобразование для эталонного рендера (как в Unreal)."""
    r = (word >> 10) & 31
    g = (word >> 5) & 31
    b = word & 31
    return ((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2))
