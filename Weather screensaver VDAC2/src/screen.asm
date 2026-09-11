; Экран заставки на FT812: весь кадр — один дисплей-лист.
;
; У TS-Conf-версии каждая часть экрана перерисовывается в видеопамяти
; отдельно. FT812 устроен иначе: он рисует кадр по дисплей-листу, и чтобы
; поменять что угодно, лист собирается заново целиком. Поэтому здесь все
; процедуры Screen_..., которые вызывает общий главный цикл
; (../shared/weather/saver.asm), только отмечают «кадр устарел», а
; Screen_Present один раз собирает новый лист (Screen_Build). Сборка — поток
; команд сопроцессору (ft812.asm): готовый неизменный блок из страницы данных
; и изменяемая часть, которую код пишет по текущему времени и погоде.
;
; Это зеркало tools/refscene.py: те же координаты (layout.inc), те же
; команды в том же порядке. Тесты сравнивают поток, записанный этим кодом, с
; потоком эталона байт в байт, а кадр, нарисованный по нему настоящим FT812,
; — с кадром эталона.

; --- слова дисплей-листа (FT81X Series Programmer Guide) ----------------------------------
DL_DISPLAY      equ #00000000
DL_BEGIN_BITMAPS equ #1F000001
DL_BEGIN_POINTS equ #1F000002
DL_BEGIN_RECTS  equ #1F000009
DL_END          equ #21000000
DL_LINE_WIDTH_8 equ #0E000008           ; полуширина линии 8/16 точки
DL_COLOR_A_FULL equ #100000FF           ; непрозрачность 255
DL_CMD_SWAP     equ #FFFFFF01           ; сопроцессор: показать собранный лист
OP_BITMAP_HANDLE equ #05                ; старший байт команды BITMAP_HANDLE
OP_CELL         equ #06                 ; CELL
OP_POINT_SIZE   equ #0D                 ; POINT_SIZE
OP_COLOR_A      equ #10                 ; COLOR_A

; Дописать в поток постоянное слово (портит HL, DE, BC, A).
        MACRO DLW value
        ld hl,#FFFF & (value)
        ld de,#FFFF & ((value) >> 16)
        call Cmd_Word
        ENDM

; Параметры строки: шрифт (адрес описания Font_...), цвет (слово COLOR_RGB),
; перо и базовая линия. Портит HL.
        MACRO TXT font, color, x, base
        ld hl,font
        ld (TxtFont),hl
        ld hl,#FFFF & (color)
        ld (TxtColor),hl
        ld hl,#FFFF & ((color) >> 16)
        ld (TxtColor+2),hl
        ld hl,x
        ld (TxtX),hl
        ld hl,base
        ld (TxtBase),hl
        ENDM

; --- вход из главного цикла ------------------------------------------------------------------
; Причина «нет погоды» показывается такой, какой она была при вызове
; Screen_Current. Главный цикл заранее кладёт в StatusText причину «на случай
; неудачи» (пока ESP отвечает, там уже «Нет данных о погоде»), а
; TS-Conf-версия перерисовывает эту строку только по Screen_Current. Кадр
; FT812 собирается каждую секунду целиком, поэтому здесь запоминается копия:
; иначе «Запрос погоды…» сменялся бы «Нет данных» ещё до ответа ESP.
Screen_Current:
        ld hl,(StatusText)
        ld (ShownStatus),hl
        ld hl,ProtoErrText              ; и доклад ESP, как он есть сейчас
        ld de,ShownError
        ld bc,PROTO_ERR_TEXT
        ldir
        ; продолжение — отметить, что кадр устарел

; Что бы ни поменялось, кадр собирается целиком в Screen_Present.
Screen_Static:
Screen_ForecastTitle:
Screen_Clock:
Screen_Colon:
Screen_Date:
Screen_Calendar:
Screen_Location:
Screen_Forecast:
        ld a,1
        ld (ScreenDirty),a
        ret

Screen_Present:
        ld a,(ScreenDirty)
        or a
        ret z                           ; ничего не менялось
        xor a
        ld (ScreenDirty),a
        ; продолжение — Screen_Build

; --- сборка кадра -----------------------------------------------------------------------------
Screen_Build:
        call Cmd_Begin
        ld a,TABLES_PAGE                ; неизменный блок и таблицы шрифтов
        call Data_Page
        ld hl,STATIC_ADDR
        ld bc,STATIC_SIZE
        call Cmd_Block                  ; фон, ручки, панели, заголовок, лента недели
        ld hl,#FFFF
        ld (TxtRight),hl                ; без обрезки
        xor a
        ld (TxtSpacing),a
        call Screen_BuildLeft
        call Screen_BuildCurrent
        call Screen_BuildForecast
        call Screen_BuildCalendar
        DLW DL_DISPLAY                  ; конец дисплей-листа
        DLW DL_CMD_SWAP                 ; показать его со следующего кадра
        jp Cmd_End

; Место, часы с мигающим двоеточием и дата.
Screen_BuildLeft:
        ld a,(WeatherValid)
        or a
        jr z,.clock                     ; погоды нет — и места не знаем
        ld hl,L_LOC_ICON_X
        ld de,L_LOC_ICON_Y
        ld a,3                          ; иконка 3 — булавка
        ld b,ICON_MINI_HANDLE
        ld c,255
        call Dl_Icon
        TXT Font_BODY, DL_ACCENT, L_LOC_TEXT_X, L_LOC_BASE
        ld hl,L_LEFT_TEXT_RIGHT
        ld (TxtRight),hl                ; длинное название не залезет на рамку
        ld hl,WeatherRecord+REC_PLACE   ; название прислала ESP, уже в CP866
        call Dl_Text
        ld hl,#FFFF
        ld (TxtRight),hl
.clock:
        call Fmt_Begin
        ld a,(RtcHour)
        call Fmt_2d
        call Fmt_End
        TXT Font_CLOCK, DL_WHITE, L_CLOCK_X, L_CLOCK_BASE
        ld hl,TextBuf
        call Dl_Text                    ; часы
        ld hl,(TxtX)
        ld (ColonX),hl                  ; перо перед двоеточием
        ld a,(RtcSec)
        rrca                            ; бит 0 в CF: нечётная секунда
        jr c,.minutes                   ; в нечётную двоеточия нет
        ld hl,StrColon
        call Dl_Text
.minutes:
        call Fmt_Begin
        ld a,(RtcMin)
        call Fmt_2d
        call Fmt_End
        ld hl,(ColonX)
        ld de,L_COLON_ADV               ; место под двоеточие — всегда
        add hl,de
        ld (TxtX),hl
        ld hl,TextBuf
        call Dl_Text                    ; минуты
        ; дата: «Четверг» и «10 сентября 2026»
        call Cal_WorkFromRtc
        call Cal_Weekday                ; A = 0..6, понедельник — 0
        push af
        TXT Font_BODY, DL_MUTED, L_TEXT_X, L_DATE1_BASE
        pop af
        ld hl,StrWeekdayFull
        call Fmt_TableEntry
        call Dl_Text
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
        call Fmt_TableEntry
        call Fmt_Str
        ld a,' '
        call Fmt_Char
        ld hl,(RtcYear)
        call Fmt_U16
        call Fmt_End
        TXT Font_BODY, DL_MUTED, L_TEXT_X, L_DATE2_BASE
        ld hl,TextBuf
        jp Dl_Text

; Текущая погода: иконка, температура, описание, закат, три строки деталей.
; Если погоды нет — причина (StatusText) и доклад ESP строкой ниже.
Screen_BuildCurrent:
        ld a,(WeatherValid)
        or a
        jp z,.status
        ; иконка по коду WMO: днём и ночью разные
        ld a,(WeatherRecord+REC_CODE)
        call Wmo_Lookup                 ; HL — запись: код, день, ночь, описание
        push hl
        inc hl                          ; поле «иконка днём»
        ld a,(WeatherRecord+REC_IS_DAY)
        or a
        jr nz,.day
        inc hl                          ; ночью — следующее поле
.day:
        ld a,(hl)
        ld hl,L_CUR_ICON_X
        ld de,L_CUR_ICON_Y
        ld b,ICON_BIG_HANDLE
        ld c,255
        call Dl_Icon
        ; температура «12°C»
        call Fmt_Begin
        ld a,(WeatherRecord+REC_TEMP)
        call Fmt_Temp
        ld a,'C'
        call Fmt_Char
        call Fmt_End
        TXT Font_TEMP, DL_WHITE, L_TEMP_X, L_TEMP_BASE
        ld hl,TextBuf
        call Dl_Text
        ; описание погоды из таблицы WMO
        TXT Font_BODY, DL_WHITE, L_TEXT_X, L_DESC_BASE
        pop hl
        inc hl
        inc hl
        inc hl                          ; поле «адрес описания»
        ld a,(hl)
        inc hl
        ld h,(hl)
        ld l,a
        call Dl_Text
        ; «Закат в 19:38»
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
        TXT Font_SMALL, DL_MUTED, L_TEXT_X, L_SUNSET_BASE
        ld hl,TextBuf
        call Dl_Text
        ; детали: осадки, ветер, давление
        call Fmt_Begin
        ld hl,(WeatherRecord+REC_PRECIP10)
        call Fmt_X10
        ld hl,Str_UNIT_PRECIP
        call Fmt_Str
        call Fmt_End
        xor a
        call Screen_Detail
        call Fmt_Begin
        ld hl,(WeatherRecord+REC_WIND10)
        call Fmt_X10
        ld hl,Str_UNIT_WIND
        call Fmt_Str
        call Fmt_End
        ld a,1
        call Screen_Detail
        call Fmt_Begin
        ld hl,(WeatherRecord+REC_PRESS)
        call Fmt_U16
        ld hl,Str_UNIT_PRESSURE
        call Fmt_Str
        call Fmt_End
        ld a,2
        jp Screen_Detail
.status:
        TXT Font_BODY, DL_MUTED, L_TEXT_X, L_DESC_BASE
        ld hl,L_LEFT_TEXT_RIGHT
        ld (TxtRight),hl
        ld hl,(ShownStatus)
        call Dl_Text
        ld a,(ShownError)               ; доклад ESP, если он был
        or a
        jr z,.no_error
        TXT Font_SMALL, DL_MUTED, L_TEXT_X, L_SUNSET_BASE
        call Screen_ErrorText           ; HL — текст без приставки «weather:»
        call Dl_Text
.no_error:
        ld hl,#FFFF
        ld (TxtRight),hl
        ret

; Строка деталей A (0..2): приглушённая мини-иконка A и текст из TextBuf.
Screen_Detail:
        ld (DetailIndex),a
        ld l,a
        ld h,0
        ld de,L_DETAIL_STEP
        call Mul_HL_DE                  ; HL = номер * шаг
        ld de,L_DETAIL_Y0
        add hl,de
        ld (DetailY),hl
        ex de,hl                        ; DE — y иконки
        ld hl,L_DETAIL_ICON_X
        ld a,(DetailIndex)
        ld b,ICON_MINI_HANDLE
        ld c,A_DETAIL_ICON
        call Dl_Icon
        ld hl,(DetailY)
        ld de,L_DETAIL_DBASE
        add hl,de
        push hl
        TXT Font_SMALL, DL_WHITE, L_DETAIL_TEXT_X, 0
        pop hl
        ld (TxtBase),hl
        ld hl,TextBuf
        jp Dl_Text

; Текст доклада ESP без служебной приставки «weather:» — на экране важна
; только причина («meteo: http 503»). Выход: HL — начало текста.
Screen_ErrorText:
        ld hl,ShownError
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
        ld hl,ShownError
        ret

; Пять строк прогноза: день, иконка, «10°…19°» у правого края, осадки под ней.
Screen_BuildForecast:
        ld a,(WeatherValid)
        or a
        ret z
        ; строк = min(5, дней - 1): первая дневная запись — сегодня
        ld a,(WeatherRecord+REC_DAY_COUNT)
        cp REC_MAX_DAYS+1
        jr c,.count_ok
        ld a,REC_MAX_DAYS
.count_ok:
        or a
        ret z
        dec a
        ret z
        cp L_FC_ROWS+1
        jr c,.rows_ok
        ld a,L_FC_ROWS
.rows_ok:
        ld (FcRows),a
        xor a
        ld (FcIndex),a
        ld hl,L_FC_ROW_Y0
        ld (FcY),hl
        ld hl,WeatherRecord+REC_DAYS+DAY_SIZE
        ld (FcEntry),hl
.row:
        ; «ПТ, 11 сен»
        call Fmt_Begin
        ld ix,(FcEntry)
        ld a,(ix+DAY_WDAY)
        ld hl,StrWeekdayShort
        call Fmt_TableEntry
        call Fmt_Str
        ld a,','
        call Fmt_Char
        ld a,' '
        call Fmt_Char
        ld ix,(FcEntry)
        ld l,(ix+DAY_DAY)
        ld h,0
        call Fmt_U16
        ld a,' '
        call Fmt_Char
        ld ix,(FcEntry)
        ld a,(ix+DAY_MONTH)
        dec a
        ld hl,StrMonthShort
        call Fmt_TableEntry
        call Fmt_Str
        call Fmt_End
        ld hl,(FcY)
        ld de,L_FC_DAY_DBASE
        add hl,de
        push hl
        TXT Font_BODY, DL_WHITE, L_FC_X, 0
        pop hl
        ld (TxtBase),hl
        ld hl,TextBuf
        call Dl_Text
        ; иконка дня
        ld ix,(FcEntry)
        ld a,(ix+DAY_CODE)
        call Wmo_Lookup
        inc hl
        ld a,(hl)                       ; иконка «днём»
        ld hl,(FcY)
        ld de,L_FC_ICON_DY
        add hl,de
        ex de,hl
        ld hl,L_FC_ICON_X
        ld b,ICON_SMALL_HANDLE
        ld c,255
        call Dl_Icon
        ; «10°…» в TextBuf2, «19°» в TextBuf
        call Fmt_Begin
        ld ix,(FcEntry)
        ld a,(ix+DAY_TMIN)
        call Fmt_Temp
        ld a,#F9                        ; многоточие в CP866
        call Fmt_Char
        call Fmt_End
        ld de,TextBuf2
        ld bc,32
        ldir                            ; копия: TextBuf займёт максимум
        call Fmt_Begin
        ld ix,(FcEntry)
        ld a,(ix+DAY_TMAX)
        call Fmt_Temp
        call Fmt_End
        ; x = правый край - ширина «10°…» - ширина «19°»
        ld hl,Font_BODY
        ld (TxtFont),hl
        ld hl,TextBuf2
        call Txt_Width
        push hl
        ld hl,TextBuf
        call Txt_Width
        pop de
        add hl,de
        ex de,hl
        ld hl,L_FC_RIGHT
        or a
        sbc hl,de
        push hl
        ld hl,(FcY)
        ld de,L_FC_TEMP_DBASE
        add hl,de
        push hl
        TXT Font_BODY, DL_MUTED, 0, 0
        pop hl
        ld (TxtBase),hl
        pop hl
        ld (TxtX),hl
        ld hl,TextBuf2
        call Dl_Text                    ; минимум серым
        ld hl,DL_WHITE & #FFFF
        ld (TxtColor),hl
        ld hl,#FFFF & (DL_WHITE >> 16)
        ld (TxtColor+2),hl
        ld hl,TextBuf
        call Dl_Text                    ; максимум белым, сразу за минимумом
        ; осадки «0.1 мм» у правого края
        call Fmt_Begin
        ld ix,(FcEntry)
        ld l,(ix+DAY_PRECIP10)
        ld h,(ix+DAY_PRECIP10+1)
        call Fmt_X10
        ld hl,Str_UNIT_MM
        call Fmt_Str
        call Fmt_End
        ld hl,Font_SMALL
        ld (TxtFont),hl
        ld hl,TextBuf
        call Txt_Width
        ex de,hl
        ld hl,L_FC_RIGHT
        or a
        sbc hl,de
        push hl
        ld hl,(FcY)
        ld de,L_FC_RAIN_DBASE
        add hl,de
        push hl
        TXT Font_SMALL, DL_RAIN, 0, 0
        pop hl
        ld (TxtBase),hl
        pop hl
        ld (TxtX),hl
        ld hl,TextBuf
        call Dl_Text
        ; разделитель — между строками, после последней его нет
        ld a,(FcIndex)
        inc a
        ld hl,FcRows
        cp (hl)
        jr nc,.next
        ld hl,(FcY)
        ld de,L_FC_SEP_DY
        add hl,de
        call Dl_Separator
.next:
        ld a,(FcIndex)
        inc a
        ld (FcIndex),a
        ld hl,FcRows
        cp (hl)
        ret nc                          ; все строки нарисованы
        ld hl,(FcY)
        ld de,L_FC_ROW_STEP
        add hl,de
        ld (FcY),hl
        ld hl,(FcEntry)
        ld de,DAY_SIZE
        add hl,de
        ld (FcEntry),hl
        jp .row

; Заголовок месяца и числа текущей недели; сегодня — голубой круг.
Screen_BuildCalendar:
        call Fmt_Begin
        ld a,(RtcMonth)
        dec a
        ld hl,StrMonthUpper
        call Fmt_TableEntry
        call Fmt_Str                    ; «СЕНТЯБРЬ»
        ld a,' '
        call Fmt_Char
        ld hl,(RtcYear)
        call Fmt_U16
        call Fmt_End
        TXT Font_CAPS, DL_MUTED, L_FC_X, L_CAL_TITLE_BASE
        ld a,L_CAPS_SPACING
        ld (TxtSpacing),a               ; заголовки — вразрядку
        ld hl,TextBuf
        call Dl_Text
        xor a
        ld (TxtSpacing),a
        ; понедельник текущей недели: от сегодня назад на «номер дня недели»
        call Cal_WorkFromRtc
        call Cal_Weekday
        or a
        jr z,.monday
        ld b,a
.back:
        push bc                         ; B — счётчик, Cal_PrevDay его портит
        call Cal_PrevDay
        pop bc
        djnz .back
.monday:
        xor a
        ld (CalCol),a
        ld hl,L_CAL_COL_X0
        ld (CalX),hl
.column:
        call Fmt_Begin
        ld a,(WorkD)
        ld l,a
        ld h,0
        call Fmt_U16
        call Fmt_End
        ld hl,Font_DAY
        ld (TxtFont),hl
        ld hl,TextBuf
        call Txt_Ink                    ; края видимых точек числа
        ; перо = x столбца + (ширина - левый - правый) / 2: середина видимых
        ; точек — на середине столбца
        ld hl,L_CAL_COL_W
        ld de,(InkLeft)
        or a
        sbc hl,de
        ld de,(InkRight)
        or a
        sbc hl,de
        sra h                           ; /2 со знаком (SRA хранит знак)
        rr l
        ld de,(CalX)
        add hl,de
        ld (NumX),hl
        call Screen_IsToday
        ld (CalActive),a
        or a
        call nz,Screen_TodayDisc
        ld a,(CalActive)
        or a
        ld hl,DL_WHITE & #FFFF
        ld de,#FFFF & (DL_WHITE >> 16)
        jr z,.color                     ; LD не меняет флаги: Z ещё от OR A
        ld hl,DL_DARK & #FFFF
        ld de,#FFFF & (DL_DARK >> 16)   ; тёмные цифры на голубом круге
.color:
        ld (TxtColor),hl
        ld (TxtColor+2),de
        ld hl,(NumX)
        ld (TxtX),hl
        ld hl,L_CAL_NUM_BASE
        ld (TxtBase),hl
        ld hl,TextBuf
        call Dl_Text
        call Cal_NextDay
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

; Круг сегодняшнего дня со свечением. Центр — ровно в центре видимых точек
; числа, в 1/16 точки: x = (перо*2 + левый + правый) * 8,
; y = (база*2 + верх + низ) * 8. Свечение — два больших полупрозрачных круга.
Screen_TodayDisc:
        ld hl,(NumX)
        add hl,hl
        ld de,(InkLeft)
        add hl,de
        ld de,(InkRight)
        add hl,de
        add hl,hl
        add hl,hl
        add hl,hl
        ld (DiscX16),hl
        ld a,(InkTop)
        call Sext_A
        ld hl,L_CAL_NUM_BASE*2
        add hl,de
        ld a,(InkBottom)
        call Sext_A
        add hl,de
        add hl,hl
        add hl,hl
        add hl,hl
        ld (DiscY16),hl
        ld hl,L_CIRCLE_R16+96
        ld a,A_GLOW/2
        call Screen_Disc
        ld hl,L_CIRCLE_R16+48
        ld a,A_GLOW
        call Screen_Disc
        ld hl,L_CIRCLE_R16
        ld a,255
        ; продолжение — Screen_Disc

; Круг цвета ACCENT радиуса HL (1/16 точки), прозрачность A, центр
; DiscX16/DiscY16. COLOR_A пишется, только если круг полупрозрачный.
Screen_Disc:
        ld (DiscR16),hl
        ld (DiscAlpha),a
        DLW DL_ACCENT
        ld a,(DiscAlpha)
        cp 255
        call nz,Dl_ColorA
        DLW DL_BEGIN_POINTS
        ld hl,(DiscR16)
        ld de,OP_POINT_SIZE << 8
        call Cmd_Word                   ; POINT_SIZE(радиус)
        ld bc,(DiscX16)
        ld de,(DiscY16)
        call Dl_Vertex16
        DLW DL_END
        ld a,(DiscAlpha)
        cp 255
        ret z
        DLW DL_COLOR_A_FULL
        ret

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

; --- примитивы: иконка, разделитель, прозрачность, вершина ------------------------------------
; Иконка: A — номер ячейки, B — ручка, C — прозрачность (255 — без COLOR_A),
; HL — x, DE — y (точки).
Dl_Icon:
        ld (IconCell),a
        ld a,b
        ld (IconHandle),a
        ld a,c
        ld (IconAlpha),a
        ld (IconX),hl
        ld (IconY),de
        DLW DL_ICON                     ; белый цвет не меняет цветов иконки
        ld a,(IconAlpha)
        cp 255
        call nz,Dl_ColorA
        DLW DL_BEGIN_BITMAPS
        ld a,(IconHandle)
        ld l,a
        ld h,0
        ld de,OP_BITMAP_HANDLE << 8
        call Cmd_Word
        ld a,(IconCell)
        ld l,a
        ld h,0
        ld de,OP_CELL << 8
        call Cmd_Word
        ld hl,(IconX)
        ld de,(IconY)
        call Dl_VertexXY
        DLW DL_END
        ld a,(IconAlpha)
        cp 255
        ret z
        DLW DL_COLOR_A_FULL
        ret

; Разделитель строк прогноза толщиной в точку на высоте HL: белый 5 %.
Dl_Separator:
        ld (SepY),hl
        DLW DL_ICON                     ; белый
        ld a,A_SEP
        call Dl_ColorA
        DLW DL_BEGIN_RECTS
        DLW DL_LINE_WIDTH_8
        ld hl,L_FC_X
        ld de,(SepY)
        call Dl_VertexXY
        ld hl,(SepY)
        inc hl
        ex de,hl
        ld hl,L_FC_RIGHT
        call Dl_VertexXY
        DLW DL_END
        DLW DL_COLOR_A_FULL
        ret

; COLOR_A(A): прозрачность следующих рисунков.
Dl_ColorA:
        ld l,a
        ld h,0
        ld de,OP_COLOR_A << 8
        jp Cmd_Word

; Вершина в точках: HL — x, DE — y (не меньше нуля).
Dl_VertexXY:
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl                       ; x * 16
        ld b,h
        ld c,l
        ex de,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl                       ; y * 16
        ex de,hl
        ; продолжение — Dl_Vertex16

; VERTEX2F: BC — x, DE — y в 1/16 точки. Слово #40000000 | x<<15 | y:
; байт 0 — y, байт 1 — старшие биты y и младший бит x, байт 2 — x>>1,
; байт 3 — #40 и x>>9.
Dl_Vertex16:
        ld l,e
        ld a,d
        and #7F
        bit 0,c
        jr z,.even
        or #80                          ; младший бит x — в старший бит байта 1
.even:
        ld h,a
        srl b
        rr c                            ; BC = x >> 1
        ld e,c                          ; байт 2
        ld a,b                          ; B = x >> 9
        and #3F
        or #40
        ld d,a                          ; байт 3
        jp Cmd_Word

; A со знаком -> DE (расширение знака: #FF в старшем байте для отрицательных).
Sext_A:
        ld e,a
        ld d,0
        bit 7,a
        ret z
        dec d
        ret

; HL = HL * DE (младшие 16 бит). Портит BC, A.
Mul_HL_DE:
        ld b,h
        ld c,l
        ld hl,0
        ld a,16
.bit:
        add hl,hl
        ex de,hl
        add hl,hl                       ; следующий бит множителя — в CF
        ex de,hl
        jr nc,.no_add
        add hl,bc
.no_add:
        dec a
        jr nz,.bit
        ret

; --- текст ------------------------------------------------------------------------------------
; Описание шрифта (Font_..., assets.inc): +0 отступ ячейки слева (pad), +1
; базовая линия в ячейке (top), +2 и +3 — ручки ячеек 0..127 и 128..255, +4
; адрес карты «код -> ячейка», +6 адрес записей ячеек (по 8 байт: шаг, dx,
; dy, левый край, правый край, верх, низ, «есть точки»). Карта и записи — в
; странице TABLES_PAGE, подключённой к #0000.

; Нарисовать строку HL: шрифт TxtFont, цвет TxtColor, перо TxtX, базовая
; линия TxtBase, разрядка TxtSpacing, правый край TxtRight (#FFFF — без
; обрезки). Символ — CELL(ячейка) и вершина левого верхнего угла ячейки;
; ручка меняется, только если символу нужна другая. Выход: TxtX — перо.
Dl_Text:
        push hl
        ld hl,(TxtColor)
        ld de,(TxtColor+2)
        call Cmd_Word                   ; COLOR_RGB
        DLW DL_BEGIN_BITMAPS
        ld a,#FF
        ld (TxtHandle),a                ; ручка ещё не выбрана
        ld ix,(TxtFont)
        pop hl
.next:
        ld a,(hl)
        or a
        jr z,.done                      ; ноль — конец строки
        inc hl
        push hl
        call Dl_Glyph
        pop hl
        jr nc,.next                     ; CF=1 — дальше правый край
.done:
        DLW DL_END
        ret

; Символ A шрифта IX. Выход: CF=1 — не влез до правого края, строка кончена.
Dl_Glyph:
        call Txt_Record                 ; HL — запись ячейки, CF=1 — символа нет
        jp c,.skip
        ; обрезка: перо + шаг > TxtRight — стоп
        ld e,(hl)
        ld d,0
        ld hl,(TxtX)
        add hl,de
        ld de,(TxtRight)
        or a
        sbc hl,de
        jr z,.fits                      ; ровно до края — помещается
        jr nc,.clip
.fits:
        ld hl,(GlyphRec)
        ld de,7
        add hl,de
        ld a,(hl)                       ; «есть точки»
        or a
        jr z,.advance                   ; пробел — только сдвиг пера
        ld a,(GlyphCell)
        ld c,(ix+2)                     ; ячейки 0..127 — первая ручка
        bit 7,a
        jr z,.handle
        ld c,(ix+3)                     ; 128..255 — вторая
.handle:
        ld a,(TxtHandle)
        cp c
        jr z,.cell
        ld a,c
        ld (TxtHandle),a
        ld l,a
        ld h,0
        ld de,OP_BITMAP_HANDLE << 8
        call Cmd_Word                   ; BITMAP_HANDLE(ручка)
.cell:
        ld a,(GlyphCell)
        and #7F
        ld l,a
        ld h,0
        ld de,OP_CELL << 8
        call Cmd_Word                   ; CELL(номер в ручке)
        ; вершина: x = перо + dx - pad, y = база - top + dy
        ld hl,(GlyphRec)
        inc hl
        ld a,(hl)                       ; dx
        call Sext_A
        ld hl,(TxtX)
        add hl,de
        ld e,(ix+0)                     ; pad
        ld d,0
        or a
        sbc hl,de
        push hl
        ld hl,(GlyphRec)
        inc hl
        inc hl
        ld a,(hl)                       ; dy
        call Sext_A
        ld hl,(TxtBase)
        add hl,de
        ld e,(ix+1)                     ; top
        ld d,0
        or a
        sbc hl,de
        ex de,hl                        ; DE — y
        pop hl                          ; HL — x
        call Dl_VertexXY
.advance:
        ld hl,(GlyphRec)
        ld e,(hl)                       ; шаг
        ld d,0
        ld hl,(TxtX)
        add hl,de
        ld a,(TxtSpacing)
        ld e,a
        add hl,de
        ld (TxtX),hl
.skip:
        or a                            ; CF=0 — дальше
        ret
.clip:
        scf
        ret

; Запись ячейки символа A шрифта IX. Выход: HL и GlyphRec — адрес записи,
; GlyphCell — номер ячейки; CF=1 — символа в шрифте нет.
Txt_Record:
        ld e,a
        ld d,0
        ld l,(ix+4)
        ld h,(ix+5)                     ; карта шрифта
        add hl,de
        ld a,(hl)                       ; номер ячейки
        inc a
        scf
        ret z                           ; #FF — символа нет
        dec a
        ld (GlyphCell),a
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl                       ; 8 байтов на запись
        ld e,(ix+6)
        ld d,(ix+7)
        add hl,de
        ld (GlyphRec),hl
        or a
        ret

; Ширина строки HL шрифтом TxtFont (сумма шагов и разрядки). Выход: HL.
Txt_Width:
        ld ix,(TxtFont)
        ld bc,0
.next:
        ld a,(hl)
        or a
        jr z,.done
        inc hl
        push hl
        push bc
        call Txt_Record
        pop bc
        jr c,.skip
        ld a,(hl)                       ; шаг
        add a,c
        ld c,a
        jr nc,.spacing
        inc b
.spacing:
        ld a,(TxtSpacing)
        add a,c
        ld c,a
        jr nc,.skip
        inc b
.skip:
        pop hl
        jr .next
.done:
        ld h,b
        ld l,c
        ret

; Края видимых точек строки HL шрифтом TxtFont (без разрядки): InkLeft —
; левый край первого символа с точками, InkRight — правый край последних
; точек, InkTop/InkBottom — самый высокий верх и самый низкий низ точек от
; базовой линии.
Txt_Ink:
        ld (InkStr),hl
        ld hl,0
        ld (InkPen),hl
        ld (InkLeft),hl
        ld (InkRight),hl
        xor a
        ld (InkFound),a
        ld a,#7F
        ld (InkTop),a                   ; ищем наименьший верх
        ld a,#80
        ld (InkBottom),a                ; и наибольший низ
.next:
        ld ix,(TxtFont)                 ; Txt_Record ищет в шрифте IX
        ld hl,(InkStr)
        ld a,(hl)
        or a
        ret z
        inc hl
        ld (InkStr),hl
        call Txt_Record
        jr c,.next
        push hl
        pop ix                          ; IX — запись ячейки
        ld a,(ix+7)
        or a
        jr z,.advance                   ; нет точек — только шаг
        ld a,(ix+3)                     ; левый край
        call Sext_A
        ld hl,(InkPen)
        add hl,de
        ld a,(InkFound)
        or a
        jr nz,.have_left
        ld (InkLeft),hl
        ld a,1
        ld (InkFound),a
.have_left:
        ld e,(ix+4)                     ; правый край (без знака)
        ld d,0
        ld hl,(InkPen)
        add hl,de
        ld de,(InkRight)
        or a
        sbc hl,de
        add hl,de
        jr c,.top                       ; не правее прежнего
        ld (InkRight),hl
.top:
        ; верх: меньшее из двух чисел со знаком. XOR #80 превращает числа
        ; со знаком в числа без знака с тем же порядком — сравнивает CP.
        ld a,(ix+5)
        xor #80
        ld c,a
        ld a,(InkTop)
        xor #80
        cp c
        jr c,.bottom                    ; прежний верх выше
        ld a,(ix+5)
        ld (InkTop),a
.bottom:
        ld a,(ix+6)
        xor #80
        ld c,a
        ld a,(InkBottom)
        xor #80
        cp c
        jr nc,.advance                  ; прежний низ ниже
        ld a,(ix+6)
        ld (InkBottom),a
.advance:
        ld e,(ix+0)                     ; шаг
        ld d,0
        ld hl,(InkPen)
        add hl,de
        ld (InkPen),hl
        jr .next

StrColon:       db ":",0
StrWeatherPrefix:
        db "weather:"
WEATHER_PREFIX_LEN equ $-StrWeatherPrefix

ScreenDirty:    db 0                    ; 1 — кадр пора собрать заново
ShownStatus:    dw Str_LOADING          ; причина на экране (снимок StatusText)
ShownError:     ds PROTO_ERR_TEXT       ; доклад ESP на экране (снимок ProtoErrText)
TxtFont:        dw 0                    ; описание шрифта (Font_...)
TxtColor:       dd 0                    ; слово COLOR_RGB
TxtX:           dw 0                    ; перо
TxtBase:        dw 0                    ; базовая линия
TxtSpacing:     db 0                    ; разрядка
TxtRight:       dw #FFFF                ; правый край, #FFFF — без обрезки
TxtHandle:      db #FF                  ; выбранная ручка в текущей строке
GlyphCell:      db 0
GlyphRec:       dw 0
ColonX:         dw 0                    ; перо перед двоеточием часов
IconCell:       db 0
IconHandle:     db 0
IconAlpha:      db 0
IconX:          dw 0
IconY:          dw 0
SepY:           dw 0
DetailIndex:    db 0
DetailY:        dw 0
FcRows:         db 0                    ; сколько строк прогноза рисовать
FcIndex:        db 0                    ; номер текущей строки
FcY:            dw 0                    ; верх текущей строки
FcEntry:        dw 0                    ; адрес дневной записи
CalCol:         db 0                    ; столбец календаря 0..6
CalX:           dw 0                    ; x левого края столбца
CalActive:      db 0                    ; 1 — столбец сегодняшнего дня
NumX:           dw 0                    ; перо числа в столбце
DiscX16:        dw 0                    ; центр круга в 1/16 точки
DiscY16:        dw 0
DiscR16:        dw 0
DiscAlpha:      db 0
InkStr:         dw 0                    ; переменные Txt_Ink
InkPen:         dw 0
InkLeft:        dw 0
InkRight:       dw 0
InkTop:         db 0
InkBottom:      db 0
InkFound:       db 0
TextBuf2:       ds 32                   ; вторая строка для температур
