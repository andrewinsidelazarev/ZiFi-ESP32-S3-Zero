; Раскладка экрана — зеркало tools/refscreen.py: те же координаты из
; layout.inc, те же строки и порядок рисования. Тесты сравнивают кадр,
; нарисованный этим кодом, с кадром эталона точка в точку.
;
; Каждая часть экрана рисуется отдельной процедурой и сначала стирает свою
; область цветом панели — так её можно перерисовать поверх старой: часы —
; раз в минуту, двоеточие — раз в секунду, погоду — при каждом запросе.
; Какие процедуры и когда вызывает общий главный цикл — см. начало
; ../shared/weather/saver.asm.

; Всё нарисованное здесь сразу видно: кадр — это сами видеостраницы. Главный
; цикл после каждой перерисовки вызывает Screen_Present (у заставки для VDAC2
; там собирается дисплей-лист FT812), и здесь делать нечего.
Screen_Present:
        ret

; --- вспомогательные макросы: параметры примитивов в ячейках памяти ------------------
; Макрос — это шаблон: ассемблер подставляет его текст вместо имени.
; Все они портят регистр HL или A: учитывайте это, если значение в регистре
; ещё понадобится после макроса.

; Записать 16-битное значение в ячейку памяти (портит HL).
        MACRO SET16 addr, value
        ld hl,value
        ld (addr),hl
        ENDM

; Записать байт в ячейку памяти (портит A).
        MACRO SET8 addr, value
        ld a,value
        ld (addr),a
        ENDM

; Задать прямоугольник для Gfx_FillRect/Gfx_Panel.
        MACRO RECT x, y, w, h
        SET16 GfxX, x
        SET16 GfxY, y
        SET16 GfxW, w
        SET16 GfxH, h
        ENDM

; Задать место, шрифт и цвета для Text_Draw (разрядка — ноль).
        MACRO TEXT_AT x, y, font, set
        SET16 TxtX, x
        SET16 TxtY, y
        SET16 TxtFont, font
        SET8 TxtSet, set
        SET8 TxtSpacing, 0
        ENDM

; --- фон и две панели -------------------------------------------------------------------
Screen_Static:
        call Gfx_Background
        RECT L_PANEL_L_X, L_PANEL_L_Y, L_PANEL_L_W, L_PANEL_L_H
        call Screen_Panel               ; левая панель
        RECT L_PANEL_R_X, L_PANEL_R_Y, L_PANEL_R_W, L_PANEL_R_H
        ; продолжение — та же панель: код ниже рисует правую

; Панель цвета PAL_PANEL с рамкой PAL_BORDER, снаружи углов — градиент фона.
Screen_Panel:
        SET8 GfxColor, PAL_PANEL
        SET8 GfxBorder, PAL_BORDER
        SET8 GfxOutside, #FF
        jp Gfx_Panel

; Стереть область GfxX..GfxH цветом панели.
Screen_ClearArea:
        SET8 GfxColor, PAL_PANEL
        jp Gfx_FillRect

; Правый край текста в левой панели: всё, что дальше, отрезается (Text_Draw
; не рисует символ, который вылез бы за TxtRight). Длинное название места или
; текст ошибки иначе залезли бы на рамку и в соседнюю панель.
; Отступ 4 px от края: области стирания доходят до этого x, и так они не
; задевают скруглённый нижний правый угол рамки.
LEFT_TEXT_RIGHT equ L_PANEL_L_X+L_PANEL_L_W-4

; --- место: булавка и название из записи погоды --------------------------------------
Screen_Location:
        ; область стирания захватывает нижние выносные элементы букв (g, p)
        RECT L_LOC_ICON_X, L_LOC_ICON_Y, LEFT_TEXT_RIGHT+1-L_LOC_ICON_X, 20
        call Screen_ClearArea
        ld a,(WeatherValid)
        or a
        ret z                           ; погоды нет — и места не знаем
        SET16 GfxX, L_LOC_ICON_X
        SET16 GfxY, L_LOC_ICON_Y
        ld hl,IconTable_mini+3*3        ; иконка 3 — булавка (3 байта на запись)
        ld a,ICON_MINI_SIZE
        call Gfx_Icon
        TEXT_AT L_LOC_TEXT_X, L_LOC_TEXT_Y, FONT_BODY, TS_ACCENT_PANEL
        ld hl,WeatherRecord+REC_PLACE   ; название прислала ESP, уже в CP866
        jp Screen_DrawClipped

; --- часы «ЧЧ:ММ» с мигающим двоеточием ---------------------------------------------
; Двоеточие видно в чётные секунды и скрыто в нечётные. Раз в минуту часы
; перерисовываются целиком; в остальные секунды меняется только двоеточие
; (Screen_Colon), чтобы цифры не мерцали.
; Место часов (L_CLOCK_X/L_CLOCK_Y) и прямоугольник цифр (L_CLOCK_BOX_...)
; вычисляет tools/gen_assets.py: часы стоят по центру левой панели.
Screen_Clock:
        RECT L_CLOCK_BOX_X, L_CLOCK_BOX_Y, L_CLOCK_BOX_W, L_CLOCK_BOX_H
        call Screen_ClearArea           ; стереть прежние цифры
        call Fmt_Begin
        ld a,(RtcHour)
        call Fmt_2d                     ; «09»
        call Fmt_End
        TEXT_AT L_CLOCK_X, L_CLOCK_Y, FONT_CLOCK, TS_WHITE_PANEL
        ld hl,TextBuf
        call Text_Draw                  ; часы
        ld hl,(TxtX)
        ld (ColonX),hl                  ; перо перед двоеточием
        call Screen_Colon
        ; минуты стоят за шириной двоеточия, видно оно сейчас или нет
        call Screen_ColonGlyph
        ld a,(hl)                       ; advance двоеточия
        ld hl,(ColonX)
        call Gfx_AddByte
        ld (TxtX),hl
        call Fmt_Begin
        ld a,(RtcMin)
        call Fmt_2d                     ; «37»
        call Fmt_End
        ld hl,TextBuf
        jp Text_Draw                    ; минуты

; Нарисовать или стереть двоеточие по чётности секунды часов.
Screen_Colon:
        ld hl,(ColonX)
        ld (TxtX),hl
        SET16 TxtY, L_CLOCK_Y
        SET8 TxtSet, TS_WHITE_PANEL
        call Screen_ColonGlyph          ; HL — глиф, CF=1 — нет в шрифте
        ret c
        ld a,(RtcSec)
        rrca                            ; бит 0 в CF: нечётная секунда
        jp nc,Gfx_Glyph                 ; чётная — нарисовать
        call Gfx_GlyphBox               ; нечётная — стереть прямоугольник глифа
        jp Screen_ClearArea

; Глиф двоеточия шрифта часов. Выход: HL и CF=0; страница шрифта подключена.
Screen_ColonGlyph:
        SET16 TxtFont, FONT_CLOCK
        ld a,FONT_PAGE
        call Data_Page
        ld a,':'
        jp Text_GlyphPtr

; --- дата: «Четверг» и «10 сентября 2026» -------------------------------------------
Screen_Date:
        ; до верха иконки погоды: вторая строка с выносными элементами («р»
        ; в «сентября») доходит до 129-й строки
        RECT L_DATE1_X, L_DATE1_Y, 128, L_CUR_ICON_Y-L_DATE1_Y
        call Screen_ClearArea
        call Cal_WorkFromRtc            ; рабочая дата = сегодня
        call Cal_Weekday                ; A = 0..6, понедельник — 0
        push af                         ; TEXT_AT портит A и HL
        TEXT_AT L_DATE1_X, L_DATE1_Y, FONT_BODY, TS_MUTED_PANEL
        pop af
        ld hl,StrWeekdayFull
        call Screen_TableEntry          ; HL — название дня недели
        call Text_Draw
        ; вторая строка: число, месяц в родительном падеже, год
        call Fmt_Begin
        ld a,(RtcDay)
        ld l,a
        ld h,0
        call Fmt_U16
        ld a,' '
        call Fmt_Char
        ld a,(RtcMonth)
        dec a                           ; месяцы 1..12, таблица с нуля
        ld hl,StrMonthGen
        call Screen_TableEntry
        call Fmt_Str
        ld a,' '
        call Fmt_Char
        ld hl,(RtcYear)
        call Fmt_U16
        call Fmt_End
        TEXT_AT L_DATE2_X, L_DATE2_Y, FONT_BODY, TS_MUTED_PANEL
        ld hl,TextBuf
        jp Text_Draw

; Элемент A таблицы указателей HL. Выход: HL — строка.
; Таблица — подряд идущие адреса по 2 байта, поэтому смещение = A * 2.
Screen_TableEntry:
        ld e,a
        ld d,0
        add hl,de
        add hl,de
        ld a,(hl)                       ; младший байт адреса
        inc hl
        ld h,(hl)                       ; старший байт
        ld l,a
        ret

; --- текущая погода или причина её отсутствия -------------------------------------
Screen_Current:
        ; до правой рамки панели: здесь бывает длинный текст ошибки
        RECT L_CUR_ICON_X, L_CUR_ICON_Y, LEFT_TEXT_RIGHT+1-L_CUR_ICON_X, 144
        call Screen_ClearArea
        ld a,(WeatherValid)
        or a
        jp z,.status                    ; данных нет — пишем причину
        ; иконка по коду WMO и признаку дня
        ld a,(WeatherRecord+REC_CODE)
        call Wmo_Lookup                 ; HL — запись таблицы
        inc hl                          ; поле «иконка днём»
        ld a,(WeatherRecord+REC_IS_DAY)
        or a
        jr nz,.day_icon
        inc hl                          ; ночная иконка
.day_icon:
        ld a,(hl)
        ld (IconId),a                   ; макросы SET16 портят HL, поэтому номер
        SET16 GfxX, L_CUR_ICON_X        ; иконки хранится отдельно
        SET16 GfxY, L_CUR_ICON_Y
        ld a,(IconId)
        ld hl,IconTable_big
        call Screen_IconEntry
        ld a,ICON_BIG_SIZE
        call Gfx_Icon
        ; температура крупно: «-12°C»
        call Fmt_Begin
        ld a,(WeatherRecord+REC_TEMP)
        call Fmt_Temp
        ld a,'C'
        call Fmt_Char
        call Fmt_End
        TEXT_AT L_TEMP_X, L_TEMP_Y, FONT_TEMP, TS_WHITE_PANEL
        ld hl,TextBuf
        call Text_Draw
        ; описание (сначала параметры вывода: TEXT_AT портит HL)
        TEXT_AT L_DESC_X, L_DESC_Y, FONT_BODY, TS_WHITE_PANEL
        ld a,(WeatherRecord+REC_CODE)
        call Wmo_Lookup
        inc hl
        inc hl
        inc hl                          ; поле «адрес описания»
        ld a,(hl)
        inc hl
        ld h,(hl)
        ld l,a
        call Text_Draw
        ; закат: «Закат в 19:38»
        call Fmt_Begin
        ld hl,Str_SUNSET_PREFIX
        call Fmt_Str
        ld a,(WeatherRecord+REC_SUNSET)
        call Fmt_2d
        ld a,':'
        call Fmt_Char
        ld a,(WeatherRecord+REC_SUNSET+1)
        call Fmt_2d
        call Fmt_End
        TEXT_AT L_SUNSET_X, L_SUNSET_Y, FONT_BODY, TS_MUTED_PANEL
        ld hl,TextBuf
        call Text_Draw
        ; детали: осадки, ветер, давление — по строке с мини-иконкой
        call Fmt_Begin
        ld hl,(WeatherRecord+REC_PRECIP10)
        call Fmt_X10                    ; осадки хранятся x10: 239 -> «23.9»
        ld hl,Str_UNIT_PRECIP
        call Fmt_Str
        call Fmt_End
        ld a,0                          ; иконка 0 — капля
        ld de,L_DETAIL_Y0
        call Screen_Detail
        call Fmt_Begin
        ld hl,(WeatherRecord+REC_WIND10)
        call Fmt_X10
        ld hl,Str_UNIT_WIND
        call Fmt_Str
        call Fmt_End
        ld a,1                          ; иконка 1 — ветер
        ld de,L_DETAIL_Y0+L_DETAIL_STEP
        call Screen_Detail
        call Fmt_Begin
        ld hl,(WeatherRecord+REC_PRESS)
        call Fmt_U16
        ld hl,Str_UNIT_PRESSURE
        call Fmt_Str
        call Fmt_End
        ld a,2                          ; иконка 2 — компас
        ld de,L_DETAIL_Y0+2*L_DETAIL_STEP
        jp Screen_Detail
.status:
        ; вместо погоды — причина («ZiFi не найден» и т. п.)
        TEXT_AT L_DESC_X, L_DESC_Y, FONT_BODY, TS_MUTED_PANEL
        ld hl,(StatusText)
        call Screen_DrawClipped
        ld a,(ProtoErrText)             ; доклад ESP, если он был
        or a
        ret z
        TEXT_AT L_SUNSET_X, L_SUNSET_Y, FONT_BODY, TS_MUTED_PANEL
        call Screen_ErrorText           ; HL — текст без приставки «weather:»
        jp Screen_DrawClipped

; Текст доклада ESP без служебной приставки «weather:» — на экране важна
; только причина («meteo: http 503»). Выход: HL — начало текста.
Screen_ErrorText:
        ld hl,ProtoErrText
        ld de,StrWeatherPrefix
        ld b,WEATHER_PREFIX_LEN
.compare:
        ld a,(de)
        cp (hl)
        jr nz,.whole                    ; приставки нет — показать всё
        inc hl
        inc de
        djnz .compare
        ret                             ; HL уже стоит за приставкой
.whole:
        ld hl,ProtoErrText
        ret

; Нарисовать строку HL в левой панели, обрезав её по LEFT_TEXT_RIGHT.
Screen_DrawClipped:
        push hl                         ; SET16 портит HL
        SET16 TxtRight, LEFT_TEXT_RIGHT
        pop hl
        call Text_Draw
        SET16 TxtRight, #FFFF           ; дальше — без ограничения
        ret

StrWeatherPrefix:
        db "weather:"
WEATHER_PREFIX_LEN equ $-StrWeatherPrefix

; Строка деталей: A — мини-иконка, DE — y, текст в TextBuf.
Screen_Detail:
        ld (GfxY),de
        ld (TxtY),de
        SET16 GfxX, L_DETAIL_ICON_X
        ld hl,IconTable_mini
        call Screen_IconEntry
        ld a,ICON_MINI_SIZE
        call Gfx_Icon
        SET16 TxtX, L_DETAIL_TEXT_X
        SET16 TxtFont, FONT_BODY
        SET8 TxtSet, TS_WHITE_PANEL
        SET8 TxtSpacing, 0
        ld hl,TextBuf
        jp Text_Draw

; Запись A таблицы иконок HL (3 байта на иконку). Выход: HL.
Screen_IconEntry:
        ld e,a
        ld d,0
        add hl,de
        add hl,de
        add hl,de                       ; HL + A*3
        ret

; --- заголовок прогноза ---------------------------------------------------------------
Screen_ForecastTitle:
        TEXT_AT L_FC_TITLE_X, L_FC_TITLE_Y, FONT_CAPS, TS_MUTED_PANEL
        SET8 TxtSpacing, 1              ; заголовки — вразрядку, как в макете
        ld hl,Str_TITLE_FORECAST
        call Text_Draw
        SET8 TxtSpacing, 0
        ret

; --- строки прогноза: завтра и ещё четыре дня ----------------------------------------
; Запись погоды содержит 6 дней, первый — сегодня; в строки идут остальные.
; Строка: «ПТ, 11 сен», иконка дня, справа «10°…19°», под ней осадки.
Screen_Forecast:
        RECT L_FC_DAY_X, L_FC_ROW_Y0-2, 170, L_FC_ROW_STEP*L_FC_ROWS+4
        call Screen_ClearArea
        ld a,(WeatherValid)
        or a
        ret z
        ld a,(WeatherRecord+REC_DAY_COUNT)
        or a
        ret z
        dec a                           ; сегодняшний день в строки не идёт
        cp L_FC_ROWS
        jr c,.rows_ok
        ld a,L_FC_ROWS                  ; не больше строк, чем помещается
.rows_ok:
        or a
        ret z
        ld (FcRows),a
        xor a
        ld (FcIndex),a
        SET16 FcY, L_FC_ROW_Y0
        SET16 FcEntry, WeatherRecord+REC_DAYS+DAY_SIZE   ; второй день записи
.row:
        ; «ПТ, 11 сен»
        call Fmt_Begin
        ld hl,(FcEntry)
        ld a,(hl)                       ; DAY_DAY
        ld (FcDay),a
        inc hl
        ld a,(hl)                       ; DAY_MONTH
        ld (FcMonth),a
        inc hl
        ld a,(hl)                       ; DAY_WDAY
        ld hl,StrWeekdayShort
        call Screen_TableEntry
        call Fmt_Str
        ld a,','
        call Fmt_Char
        ld a,' '
        call Fmt_Char
        ld a,(FcDay)
        ld l,a
        ld h,0
        call Fmt_U16
        ld a,' '
        call Fmt_Char
        ld a,(FcMonth)
        dec a
        ld hl,StrMonthShort
        call Screen_TableEntry
        call Fmt_Str
        call Fmt_End
        ld hl,(FcY)
        ld de,L_FC_DAY_DY
        add hl,de
        ld (TxtY),hl
        SET16 TxtX, L_FC_DAY_X
        SET16 TxtFont, FONT_BODY
        SET8 TxtSet, TS_WHITE_PANEL
        SET8 TxtSpacing, 0
        ld hl,TextBuf
        call Text_Draw
        ; иконка дня
        ld hl,(FcEntry)
        ld de,DAY_CODE
        add hl,de
        ld a,(hl)
        call Wmo_Lookup
        inc hl
        ld a,(hl)                       ; дневная иконка
        ld (IconId),a
        SET16 GfxX, L_FC_ICON_X
        ld hl,(FcY)
        ld de,L_FC_ICON_DY
        add hl,de
        ld (GfxY),hl
        ld a,(IconId)
        ld hl,IconTable_small
        call Screen_IconEntry
        ld a,ICON_SMALL_SIZE
        call Gfx_Icon
        ; температуры справа: минимум серым с многоточием, максимум белым.
        ; Строки две (разные цвета), а прижать к краю надо обе вместе,
        ; поэтому сначала меряем их общую ширину.
        call Fmt_Begin
        ld hl,(FcEntry)
        ld de,DAY_TMIN
        add hl,de
        ld a,(hl)
        call Fmt_Temp
        ld hl,Str_DOTS
        call Fmt_Str
        call Fmt_End
        ld hl,TextBuf
        ld de,TextBuf2
        ld bc,32
        ldir                            ; TextBuf2 — «10°…»
        call Fmt_Begin
        ld hl,(FcEntry)
        ld de,DAY_TMAX
        add hl,de
        ld a,(hl)
        call Fmt_Temp
        call Fmt_End                    ; TextBuf — «19°»
        SET16 TxtFont, FONT_BODY
        ld hl,TextBuf2
        call Text_Width
        ld (FcWidth),hl
        ld hl,TextBuf
        call Text_Width
        ld de,(FcWidth)
        add hl,de                       ; общая ширина
        ex de,hl
        ld hl,L_FC_RIGHT
        or a
        sbc hl,de                       ; начало = правый край - ширина
        ld (TxtX),hl
        ld hl,(FcY)
        ld de,L_FC_TEMP_DY
        add hl,de
        ld (TxtY),hl
        SET8 TxtSet, TS_MUTED_PANEL
        ld hl,TextBuf2
        call Text_Draw                  ; перо остаётся после «…»
        SET8 TxtSet, TS_WHITE_PANEL
        ld hl,TextBuf
        call Text_Draw
        ; осадки под температурой, капителью
        call Fmt_Begin
        ld hl,(FcEntry)
        ld de,DAY_PRECIP10
        add hl,de
        ld a,(hl)
        inc hl
        ld h,(hl)
        ld l,a
        call Fmt_X10
        ld hl,Str_UNIT_MM_UPPER
        call Fmt_Str
        call Fmt_End
        SET16 TxtX, L_FC_RIGHT
        ld hl,(FcY)
        ld de,L_FC_RAIN_DY
        add hl,de
        ld (TxtY),hl
        SET16 TxtFont, FONT_CAPS
        SET8 TxtSet, TS_RAIN_PANEL
        ld hl,TextBuf
        call Text_DrawRight
        ; разделитель между строками
        ld a,(FcIndex)
        inc a
        ld (FcIndex),a
        ld hl,FcRows
        cp (hl)
        ret nc                          ; последняя строка
        SET16 GfxX, L_FC_SEP_X0
        SET16 GfxW, L_FC_SEP_X1-L_FC_SEP_X0+1
        ld hl,(FcY)
        ld de,L_FC_SEP_DY
        add hl,de
        ld (GfxY),hl
        SET8 GfxColor, PAL_SEP
        call Gfx_HLine
        ld hl,(FcY)
        ld de,L_FC_ROW_STEP
        add hl,de
        ld (FcY),hl                     ; следующая строка ниже
        ld hl,(FcEntry)
        ld de,DAY_SIZE
        add hl,de
        ld (FcEntry),hl                 ; следующий день записи
        jp .row

; --- календарь: заголовок месяца и лента текущей недели --------------------------
Screen_Calendar:
        ; стереть от заголовка месяца до низа ленты
        RECT L_CAL_TITLE_X, L_CAL_TITLE_Y, L_STRIP_W, L_STRIP_Y+L_STRIP_H-L_CAL_TITLE_Y
        call Screen_ClearArea
        call Fmt_Begin
        ld a,(RtcMonth)
        dec a
        ld hl,StrMonthUpper
        call Screen_TableEntry
        call Fmt_Str                    ; «СЕНТЯБРЬ»
        ld a,' '
        call Fmt_Char
        ld hl,(RtcYear)
        call Fmt_U16
        call Fmt_End
        TEXT_AT L_CAL_TITLE_X, L_CAL_TITLE_Y, FONT_CAPS, TS_MUTED_PANEL
        SET8 TxtSpacing, 1
        ld hl,TextBuf
        call Text_Draw
        SET8 TxtSpacing, 0
        ; лента — вложенная панель без видимой рамки
        RECT L_STRIP_X, L_STRIP_Y, L_STRIP_W, L_STRIP_H
        SET8 GfxColor, PAL_STRIP
        SET8 GfxBorder, PAL_STRIP
        SET8 GfxOutside, PAL_PANEL      ; снаружи углов ленты — цвет панели
        call Gfx_Panel
        SET8 GfxOutside, #FF
        ; понедельник текущей недели: от сегодняшней даты назад на
        ; «номер дня недели» дней
        call Cal_WorkFromRtc
        call Cal_Weekday
        or a
        jr z,.monday                    ; сегодня и есть понедельник
        ld b,a
.back:
        push bc                         ; B — счётчик, Cal_PrevDay его портит
        call Cal_PrevDay
        pop bc
        djnz .back
.monday:
        xor a
        ld (CalCol),a
        SET16 CalX, L_CAL_COL_X0
.column:
        ; подпись дня недели
        ld a,(CalCol)
        ld hl,StrWeekdayShort
        call Screen_TableEntry
        push hl
        ld hl,(CalX)
        ld (TxtX),hl
        SET16 TxtY, L_CAL_LABEL_Y
        SET16 TxtFont, FONT_CAPS
        SET8 TxtSpacing, 0
        SET8 TxtSet, TS_MUTED_STRIP     ; будни — серым
        ld a,(CalCol)
        cp 5                            ; столбцы 5 и 6 — суббота и воскресенье
        jr c,.label_color
        SET8 TxtSet, TS_RED_STRIP       ; выходные — красным
.label_color:
        pop hl
        ld de,L_CAL_COL_W
        call Text_DrawCentered
        ; число дня — в TextBuf: по нему выбирается и круг сегодняшнего дня
        call Fmt_Begin
        ld a,(WorkD)
        ld l,a
        ld h,0
        call Fmt_U16
        call Fmt_End
        SET16 TxtFont, FONT_BODY
        ; сегодняшний день — голубой круг под числом
        call Screen_IsToday
        ld (CalActive),a
        or a
        call nz,Screen_TodayDisc        ; CALL NZ — вызов, только если Z=0
        ld hl,(CalX)
        ld (TxtX),hl
        SET16 TxtY, L_CAL_NUM_Y
        ld a,(CalActive)
        or a
        ld a,TS_WHITE_STRIP             ; LD не меняет флаги: Z ещё от OR A
        jr z,.number_color
        ld a,TS_DARK_ACCENT             ; тёмные цифры на голубом круге
.number_color:
        ld (TxtSet),a
        ld hl,TextBuf
        ld de,L_CAL_COL_W
        call Text_DrawCentered
        call Cal_NextDay                ; следующий день недели
        ld hl,(CalX)
        ld de,L_CAL_COL_W
        add hl,de
        ld (CalX),hl
        ld a,(CalCol)
        inc a
        ld (CalCol),a
        cp 7
        jp c,.column                    ; семь столбцов
        ret

; Голубой круг сегодняшнего дня под числом из TextBuf (шрифт — TxtFont).
; Центр круга должен совпасть с центром рисунка цифр. По высоте это решено
; заранее: верх круга L_CAL_CIRCLE_Y вычислил tools/gen_assets.py по
; метрикам цифр. По ширине Text_DrawCentered ставит центр рисунка на
; середину столбца, если ширина рисунка чётная («10» — 12 точек), и на
; полточки левее, если нечётная («21» — 13 точек). Поэтому кругов два:
; DISC_EVEN и DISC_ODD; сглаженный край позволяет поставить центр круга и
; между точками. Сдвиг круга от x столбца уже записан в поле left глифа.
Screen_TodayDisc:
        ld hl,TextBuf
        call Text_InkBounds             ; DE — левый край точек, HL — правый
        ld a,e
        add a,l                         ; левый + правый: та же чётность, что
                                        ; у ширины (правый - левый)
        rrca                            ; бит 0 — в флаг CF
        ld hl,DISC_EVEN
        jr nc,.draw                     ; CF=0 — ширина чётная
        ld hl,DISC_ODD
.draw:
        push hl                         ; SET16 и SET8 портят HL и A
        ld hl,(CalX)
        ld (TxtX),hl
        SET16 TxtY, L_CAL_CIRCLE_Y
        SET8 TxtSet, TS_ACCENT_STRIP
        ld a,DISC_PAGE
        call Data_Page                  ; страница с кругами — в окно #0000
        pop hl
        jp Gfx_Glyph

; Совпадает ли рабочая дата с датой часов. Выход: A=1 — да, 0 — нет.
Screen_IsToday:
        ld a,(WorkD)
        ld hl,RtcDay
        cp (hl)
        jr nz,.no
        ld a,(WorkM)
        ld hl,RtcMonth
        cp (hl)
        jr nz,.no
        ld hl,(WorkY)
        ld de,(RtcYear)
        or a
        sbc hl,de
        jr nz,.no
        ld a,1
        ret
.no:
        xor a
        ret

IconId:         db 0                    ; номер иконки между макросами
ColonX:         dw 0                    ; перо перед двоеточием часов
FcRows:         db 0                    ; сколько строк прогноза рисовать
FcIndex:        db 0                    ; номер текущей строки
FcY:            dw 0                    ; y текущей строки
FcEntry:        dw 0                    ; адрес дневной записи
FcDay:          db 0
FcMonth:        db 0
FcWidth:        dw 0                    ; ширина «10°…»
CalCol:         db 0                    ; столбец календаря 0..6
CalX:           dw 0                    ; x левого края столбца
CalActive:      db 0                    ; 1 — столбец сегодняшнего дня
TextBuf2:       ds 32                   ; вторая строка для температур
