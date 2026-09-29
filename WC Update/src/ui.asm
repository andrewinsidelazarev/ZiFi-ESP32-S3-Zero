; Окно обновлятора: состояние, полоса хода, заголовок колонок и список
; файлов с отметками.
;
; Рисование разбито на шаги по одной строке (Ui_DrawStep), и главный цикл
; делает шаг, только когда очередь ZiFi пуста. У ZiFi нет управления потоком,
; а ESP шлёт окно записи 16 КиБ сплошным потоком: долгая перерисовка всего
; окна за раз переполнила бы 256-байтовую очередь приёма. События от ESP лишь
; помечают строки к перерисовке.
;
; PRSRW пишет прямо в экран в окне #C000 и TXT сам не подключает, поэтому
; перед выводом TXT подключается явно (API 0, #FE): в #C000 может стоять
; страница таблицы или окно записи vfs.asm. Текст строки собирается в RowBuf
; внутри страницы кода — из #C000 PRSRW читал бы экран.

UI_W            equ 72                  ; ширина окна с рамкой
UI_IW           equ UI_W-2              ; ширина строки внутри рамки
UI_H            equ 19                  ; высота с рамкой — как у окна FTP-сервера
UI_H_MIN        equ 12                  ; на экране ниже 21 строки окно ужимается
UI_HEAD         equ 3                   ; строк над списком
UI_MAX_ROWS     equ UI_H-2-UI_HEAD      ; 14 строк списка
UI_LINE_STATUS  equ 1
UI_LINE_BAR     equ 2
UI_LINE_HEAD    equ 3
UI_LINE_LIST    equ 4                   ; первая строка списка
UI_KEY_WAIT     equ 10                  ; кадров отложенная клавиша ждёт перерисовку
; Отложенные клавиши (UiNavKey): применяются, когда окно дорисовано.
UI_KEY_UP       equ 1
UI_KEY_DOWN     equ 2
UI_KEY_PGUP     equ 3
UI_KEY_PGDN     equ 4
UI_KEY_HOME     equ 5
UI_KEY_END      equ 6
UI_KEY_SPACE    equ 7
UI_BAR_CELLS    equ 50                  ; 2% на клетку

UI_COLOR        equ #17                 ; белый по синему, как у FTP
UI_COLOR_CURSOR equ #50                 ; чёрный по голубому
UI_COLOR_HEAD   equ #1D
UI_COLOR_WARN   equ #1E                 ; отличается, новый
UI_COLOR_BAD    equ #1A                 ; ошибка
UI_COLOR_GOOD   equ #1C                 ; обновлён

UI_BAR_FULL     equ #DB                 ; те же знаки, что у полос WC
UI_BAR_EMPTY    equ #B0

; Колонки строки списка в RowBuf (с нуля).
UI_COL_MARK     equ 1                   ; "[x]"
UI_COL_NAME     equ 5
UI_NAME_W       equ 36
UI_COL_SD       equ 42
UI_COL_GH       equ 50
UI_NUM_W        equ 7
UI_COL_STATE    equ 58
UI_STATE_W      equ 8

; Открыть окно и вывести заголовок колонок. Высота постоянная, как у окна
; FTP-сервера: 19 строк с рамкой влезают в любой текстовый режим WC (25, 30,
; 36 строк) вместе с тенью. Экран ниже 21 строки (или странное значение
; высоты) — окно ужимается до экрана, но не ниже UI_H_MIN.
Ui_Open:
        ld a,(WC_HEI)
        sub 2
        jr c,.small
        cp UI_H+1
        jr c,.fits
        ld a,UI_H
.fits:
        cp UI_H_MIN
        jr nc,.height
.small:
        ld a,UI_H_MIN
.height:
        ld (WcuWindow+5),a              ; высота окна
        sub 2+UI_HEAD
        ld (UiRows),a                   ; строк списка: 7..14
        ; X/Y=#FF PRWOW заменяет центром текущего режима и пишет обратно в
        ; описатель, а адрес фона от прошлого запуска к нынешнему экрану
        ; отношения не имеет — всё это ставится заново перед каждым показом.
        ld ix,WcuWindow
        ld (ix+2),#FF
        ld (ix+3),#FF
        ld (ix+8),0
        ld (ix+9),0
        call WC_PRWOW
        ld hl,UiHeadText
        ld d,UI_LINE_HEAD
        ld a,UI_COLOR_HEAD
        call Ui_PrintLine
        ld a,1
        ld (UiDirtyBar),a
        jp Ui_DirtyAll

; HL — строка UI_IW символов, D — строка окна, A — цвет.
Ui_PrintLine:
        ld e,1
        ld bc,UI_IW
; HL — текст, BC — длина, D/E — строка и колонка окна, A — цвет.
Ui_Print:
        ld (UiColor),a
        push hl
        push bc
        push de
        ld a,#FE
        call WC_MNGC_PL                 ; TXT в #C000: PRSRW пишет прямо туда
        pop de
        pop bc
        pop hl
        ld ix,WcuWindow
        call WC_PRSRW                   ; HL и B остаются для PRIAT
        ld a,(UiColor)
        jp WC_PRIAT

; --- строка состояния ---------------------------------------------------------

; HL — текст с нулём. Перед ним — UiStatusPrefix, после — UiStatusSuffix
; (0 — нет). Строка помечается к перерисовке.
Ui_SetStatus:
        push hl
        ld hl,UiStatusBuf
        ld de,UiStatusBuf+1
        ld bc,UI_IW-1
        ld (hl),' '
        ldir
        ld de,UiStatusBuf
        ld b,UI_IW
        ld hl,(UiStatusPrefix)
        call Ui_Append
        pop hl
        call Ui_Append
        ld hl,(UiStatusSuffix)
        call Ui_Append
        ; Ошибка — красным.
        ld hl,UiStatusBuf
        ld de,UiErrorWord
        ld b,6                          ; "ERROR:"
.compare:
        ld a,(de)
        cp (hl)
        jr nz,.normal
        inc hl
        inc de
        djnz .compare
        ld a,UI_COLOR_BAD
        jr .color
.normal:
        ld a,UI_COLOR
.color:
        ld (UiStatusColor),a
        ld a,1
        ld (UiDirtyStatus),a
        ret

; Дописать строку HL (0 — нет строки) в поле DE, где осталось B мест.
; Управляющие байты заменяются точками.
Ui_Append:
        ld a,h
        or l
        ret z
.copy:
        ld a,b
        or a
        ret z
        ld a,(hl)
        or a
        ret z
        cp ' '
        jr nc,.put
        ld a,'.'
.put:
        ld (de),a
        inc de
        inc hl
        dec b
        jr .copy

; Показать состояние сразу: до START линия молчит, ждать нечего.
Ui_StatusNow:
        call Ui_SetStatus
        xor a
        ld (UiDirtyStatus),a
Ui_DrawStatus:
        ld hl,UiStatusBuf
        ld d,UI_LINE_STATUS
        ld a,(UiStatusColor)
        jp Ui_PrintLine

; --- полоса хода --------------------------------------------------------------

; " [####....] 47%  12/30": процент — от ESP, счётчик — файлы этапа.
Ui_DrawBar:
        call Ui_ClearRow
        ld a,'['
        ld (RowBuf+1),a
        ld a,(WcuPercent)
        srl a
        ld c,a                          ; полных клеток
        ld hl,RowBuf+2
        ld b,UI_BAR_CELLS
.cell:
        ld a,c
        or a
        ld a,UI_BAR_EMPTY
        jr z,.put
        dec c
        ld a,UI_BAR_FULL
.put:
        ld (hl),a
        inc hl
        djnz .cell
        ld (hl),']'
        inc hl
        inc hl
        ex de,hl
        ld a,(WcuPercent)
        ld l,a
        ld h,0
        call Ui_Dec16
        ld a,'%'
        ld (de),a
        inc de
        ld hl,(WcuTot)
        ld a,h
        or l
        jr z,.print
        inc de
        inc de
        ld hl,(WcuCur)
        call Ui_Dec16
        ld a,'/'
        ld (de),a
        inc de
        ld hl,(WcuTot)
        call Ui_Dec16
.print:
        ld hl,RowBuf
        ld d,UI_LINE_BAR
        ld a,UI_COLOR
        jp Ui_PrintLine

; RowBuf — пробелами.
Ui_ClearRow:
        ld hl,RowBuf
        ld de,RowBuf+1
        ld bc,UI_IW-1
        ld (hl),' '
        ldir
        ret

; --- список -------------------------------------------------------------------

; Один шаг перерисовки: состояние, полоса, первая помеченная строка списка
; или полоса прокрутки. Выход: NZ — что-то нарисовано, Z — всё на экране.
Ui_DrawStep:
        ld hl,UiDirtyStatus
        ld a,(hl)
        or a
        jr z,.bar
        ld (hl),0
        call Ui_DrawStatus
        jr .drew
.bar:
        ld hl,UiDirtyBar
        ld a,(hl)
        or a
        jr z,.rows
        ld (hl),0
        call Ui_DrawBar
        jr .drew
.rows:
        ld hl,UiRowDirty
        ld a,(UiRows)
        ld b,a
        ld c,0
.find:
        ld a,(hl)
        or a
        jr nz,.row
        inc hl
        inc c
        djnz .find
        ld hl,UiDirtyScroll
        ld a,(hl)
        or a
        ret z                           ; рисовать нечего
        ld (hl),0
        call Ui_DrawScroll
        jr .drew
.row:
        ld (hl),0
        ld a,c
        call Ui_DrawRow
.drew:
        or 1                            ; NZ
        ret

; A — строка окна списка (с нуля).
Ui_DrawRow:
        ld (UiRowNo),a
        ld hl,UiTop
        add a,(hl)
        ld (UiRowIndex),a
        ld hl,WcuCount
        cp (hl)
        jr c,.entry
        call Ui_ClearRow                ; за концом списка — пустая строка
        ld a,UI_COLOR
        ld (UiStateColor),a
        ld (UiRowColor),a
        jr .print
.entry:
        call Ui_BuildRow
        ld a,(UiRowIndex)
        ld hl,UiCursor
        cp (hl)
        ld a,UI_COLOR
        jr nz,.color
        ld a,UI_COLOR_CURSOR
.color:
        ld (UiRowColor),a
.print:
        ld a,(UiRowNo)
        add a,UI_LINE_LIST
        ld (UiRowLine),a
        ld d,a
        ld hl,RowBuf
        ld a,(UiRowColor)
        call Ui_PrintLine
        ; Состояние — своим цветом; строка курсора — целиком цветом курсора.
        ld a,(UiRowColor)
        cp UI_COLOR_CURSOR
        ret z
        ld a,(UiStateColor)
        cp UI_COLOR
        ret z
        ld hl,RowBuf+UI_COL_STATE
        ld bc,UI_STATE_W
        ld a,(UiRowLine)
        ld d,a
        ld e,UI_COL_STATE+1
        ld a,(UiStateColor)
        jp Ui_Print

; Собрать RowBuf из строки таблицы UiRowIndex и выбрать цвет состояния.
Ui_BuildRow:
        call Wcu_MapTable
        ld a,(UiRowIndex)
        call Wcu_EntryAddr
        push hl
        pop ix                          ; IX -> строка в странице 2 (#C000)
        call Ui_ClearRow
        ; Отметка — только у файлов, которые можно обновить.
        ld a,(ix+E_FLAGS)
        bit WCU_B_CAN,a
        jr z,.name
        ld hl,RowBuf+UI_COL_MARK
        ld (hl),'['
        inc hl
        ld (hl),' '
        bit WCU_B_MARK,a
        jr z,.close
        ld (hl),'x'
.close:
        inc hl
        ld (hl),']'
.name:
        ; Путь длиннее поля — показать хвост, где имя файла, с '<' в начале.
        ld a,(ix+E_NAMELEN)
        ld c,a
        ld b,0
        push ix
        pop hl
        ld de,E_NAME
        add hl,de
        ld de,RowBuf+UI_COL_NAME
        cp UI_NAME_W+1
        jr c,.copy_name
        sub UI_NAME_W-1
        ld c,a
        add hl,bc
        ld a,'<'
        ld (de),a
        inc de
        ld bc,UI_NAME_W-1
.copy_name:
        ld a,b
        or c
        jr z,.sizes
        ldir
.sizes:
        push ix
        pop hl
        ld de,E_LOCAL
        add hl,de
        ld de,RowBuf+UI_COL_SD
        ld a,(ix+E_FLAGS)
        and WCU_F_LOCAL
        call Ui_Size
        push ix
        pop hl
        ld de,E_REMOTE
        add hl,de
        ld de,RowBuf+UI_COL_GH
        ld a,(ix+E_FLAGS)
        and WCU_F_REMOTE
        call Ui_Size
        ; Слово состояния и его цвет: 8 символов и байт цвета на запись.
        ld a,(ix+E_STATUS)
        cp WCU_ST_FAILED+1
        jr c,.known
        xor a
.known:
        ld l,a
        ld h,0
        ld e,l
        ld d,h
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,de                       ; *9
        ld de,UiStateTable
        add hl,de
        ld de,RowBuf+UI_COL_STATE
        ld bc,UI_STATE_W
        ldir
        ld a,(hl)
        ld (UiStateColor),a
        ret

; Z — файла на этой стороне нет: "-". Иначе HL -> размер LE24, DE -> поле.
Ui_Size:
        jr nz,Ui_U24
        ex de,hl
        ld bc,UI_NUM_W-1
        add hl,bc
        ld (hl),'-'
        ret

; HL -> число LE24, DE -> поле UI_NUM_W символов: число по правому краю.
; Восемь цифр в поле не влезают — тогда размер в КиБ с буквой K. #FFFFFF —
; «16 МиБ и больше» (ESP насыщает поле): >16M.
Ui_U24:
        push de
        ld de,UiNumber
        ld bc,3
        ldir
        ld hl,UiNumber
        ld a,(hl)
        inc hl
        and (hl)
        inc hl
        and (hl)
        inc a
        jr nz,.number
        ld hl,UiHuge
        ld de,UiDigits
        ld bc,UI_HUGE_LEN
        ldir
        ld a,UI_HUGE_LEN
        jr .fits
.number:
        ld de,UiDigits
        call Ui_Dec24                   ; DE — за последней цифрой
        ex de,hl
        ld de,UiDigits
        or a
        sbc hl,de
        ld a,l                          ; цифр: 1..8
        cp UI_NUM_W+1
        jr c,.fits
        ; число >> 10: байты 1..2 числа, сдвинутые на два бита
        ld hl,(UiNumber+1)
        srl h
        rr l
        srl h
        rr l
        ld (UiNumber),hl
        xor a
        ld (UiNumber+2),a
        ld de,UiDigits
        call Ui_Dec24
        ld a,'K'
        ld (de),a
        inc de
        ex de,hl
        ld de,UiDigits
        or a
        sbc hl,de
        ld a,l
.fits:
        pop de
        ld c,a
        ld a,UI_NUM_W
        sub c
        add a,e
        ld e,a
        jr nc,.place
        inc d
.place:
        ld hl,UiDigits
        ld b,0
        ldir
        ret

; HL — число, DE — куда писать цифры. Выход: DE — за последней цифрой.
Ui_Dec16:
        ld (UiNumber),hl
        xor a
        ld (UiNumber+2),a
; Число UiNumber (3 байта LE) — в DE десятичными цифрами без ведущих нулей
; (ноль — «0»). Выход: DE — за последней цифрой. Разрушает AF, AF', BC, HL.
; Степени десяти вычитаются 16-битным SBC, не больше девяти раз на цифру.
; U32_ToDec из fs.asm делит на 10 побайтным вычитанием — около 5000 тактов на
; цифру; строка списка с двумя размерами стоила с ним 135 тысяч тактов, а
; очередь ZiFi наполняется за 77 тысяч (22 мс при 3,5 МГц).
Ui_Dec24:
        ld (UiOut),de
        ld hl,UiPow10
        ld (UiPowPtr),hl
        ld a,UI_DEC_DIGITS
        ld (UiDigitsLeft),a
        xor a
        ld (UiStarted),a
        ld hl,(UiNumber)
        ld a,(UiNumber+2)
        ld c,a                          ; C:HL — число
.digit:
        push hl
        ld hl,(UiPowPtr)
        ld e,(hl)
        inc hl
        ld d,(hl)
        inc hl
        ld b,(hl)
        inc hl
        ld (UiPowPtr),hl
        pop hl                          ; B:DE — степень десяти
        xor a
        ex af,af'                       ; A' — цифра
.subtract:
        or a
        sbc hl,de
        ld a,c
        sbc a,b
        jr c,.restore
        ld c,a
        ex af,af'
        inc a
        ex af,af'
        jr .subtract
.restore:
        add hl,de                       ; старший байт C не записывался
        ex af,af'
        ld b,a                          ; цифра 0..9
        ld a,(UiDigitsLeft)
        dec a
        ld (UiDigitsLeft),a
        jr z,.emit                      ; последняя цифра — всегда
        ld a,b
        or a
        jr nz,.emit
        ld a,(UiStarted)
        or a
        jr z,.digit                     ; ведущий ноль
.emit:
        ld a,1
        ld (UiStarted),a
        push hl
        ld hl,(UiOut)
        ld a,b
        add a,'0'
        ld (hl),a
        inc hl
        ld (UiOut),hl
        pop hl
        ld a,(UiDigitsLeft)
        or a
        jr nz,.digit
        ld de,(UiOut)
        ret

UI_DEC_DIGITS   equ 8                   ; 24 бита — до 16 777 215
UiPow10:
        db 10000000 & #FF, (10000000 >> 8) & #FF, 10000000 >> 16
        db 1000000 & #FF, (1000000 >> 8) & #FF, 1000000 >> 16
        db 100000 & #FF, (100000 >> 8) & #FF, 100000 >> 16
        db 10000 & #FF, 10000 >> 8, 0
        db 1000 & #FF, 1000 >> 8, 0
        db 100, 0, 0
        db 10, 0, 0
        db 1, 0, 0
        ASSERT $-UiPow10 == 3*UI_DEC_DIGITS

; Полоса прокрутки на правой рамке, как в меню F10: дорожка и ползунок.
; Строка ползунка — TOP*(ROWS-1)/(COUNT-ROWS). Список влез — полосы нет.
Ui_DrawScroll:
        ld a,(UiRows)
        ld b,a
        ld a,(WcuCount)
        sub b
        ret c
        ret z
        ld c,a                          ; COUNT-ROWS
        ld a,(UiTop)
        ld e,a
        ld d,0
        ld hl,0
        dec b
        jr z,.divide
.multiply:
        add hl,de
        djnz .multiply
.divide:
        ld e,c
        ld d,0
        ld b,0
.subtract:
        or a
        sbc hl,de
        jr c,.knob
        inc b
        jr .subtract
.knob:
        ld c,b                          ; строка ползунка
        ld a,(UiRows)
        ld b,a
        ld hl,RowBuf
        ld e,0
.cell:
        ld a,e
        cp c
        ld a,UI_BAR_EMPTY
        jr nz,.put
        ld a,UI_BAR_FULL
.put:
        ld (hl),a
        inc hl
        ld (hl),#0D                     ; TXTPR: вниз, в ту же колонку
        inc hl
        inc e
        djnz .cell
        dec hl
        ld (hl),0                       ; последний #0D — конец строки
        ld ix,WcuWindow
        ld hl,RowBuf
        ld d,UI_LINE_LIST
        ld e,UI_IW+1                    ; правая рамка окна
        jp WC_TXTPR                     ; TXTPR сам подключает TXT

; Пометить к перерисовке строку таблицы A, если она видна.
Ui_DirtyIndex:
        ld hl,UiTop
        sub (hl)
        ret c
        ld hl,UiRows
        cp (hl)
        ret nc
        ld e,a
        ld d,0
        ld hl,UiRowDirty
        add hl,de
        ld (hl),1
        ret

; Пометить к перерисовке весь список и полосу прокрутки.
Ui_DirtyAll:
        ld hl,UiRowDirty
        ld de,UiRowDirty+1
        ld bc,UI_MAX_ROWS-1
        ld (hl),1
        ldir
        ld a,1
        ld (UiDirtyScroll),a
        ret

; --- клавиши ------------------------------------------------------------------

; Раз в кадр, всегда. WC хранит только текущее состояние клавиши (отпускание
; очищает ячейку), и нажатие, не спрошенное в свой кадр, пропало бы. Стрелки,
; листание и пробел запоминаются (UiNavKey) и применяются, когда окно
; дорисовано: иначе удержанная стрелка сдвигает список каждый кадр, а
; перерисовка всех строк занимает около кадра, и нижние строки так и не
; обновляются. Так прокрутка идёт не быстрее перерисовки, а короткое нажатие
; не теряется. Дольше UI_KEY_WAIT кадров не ждём: пока линия занята,
; рисовать нельзя. Enter, A и Esc действуют сразу.
Ui_KeysFrame:
        call Ui_Keys
        ld a,(UiNavKey)
        or a
        ret z
        call Ui_Pending
        jr z,Ui_Flush                   ; окно дорисовано
        ld hl,UiKeyWait
        inc (hl)
        ld a,(hl)
        cp UI_KEY_WAIT
        ret c
; Применить отложенную клавишу, если она есть.
Ui_Flush:
        ld a,(UiNavKey)
        or a
        ret z
        ld b,a
        xor a
        ld (UiNavKey),a
        ld (UiKeyWait),a
        ld a,b
        dec a
        jp z,Ui_Up
        dec a
        jp z,Ui_Down
        dec a
        jp z,Ui_PageUp
        dec a
        jp z,Ui_PageDown
        dec a
        jp z,Ui_Home
        dec a
        jp z,Ui_End
        jp Ui_Space

; Запомнить клавишу A (UI_KEY_*). Та же ещё ждёт — повтор не копится, и
; удержанная клавиша не обгоняет перерисовку; другая — прежняя сначала
; применяется, чтобы порядок нажатий сохранился.
Ui_Latch:
        ld hl,UiNavKey
        cp (hl)
        ret z
        push af
        call Ui_Flush
        pop af
        ld (UiNavKey),a
        ret

; NZ — на экране что-то ещё не нарисовано.
Ui_Pending:
        ld a,(UiDirtyStatus)
        ld hl,UiDirtyBar
        or (hl)
        ld hl,UiDirtyScroll
        or (hl)
        ret nz
        ld a,(UiRows)
        ld b,a
        ld hl,UiRowDirty
.row:
        ld a,(hl)
        or a
        ret nz
        inc hl
        djnz .row
        ret

; Опрос клавиш WC (из Ui_KeysFrame). Автоповтор WC общий для всех клавиш:
; первое нажатие даёт NZ, затем пауза и повтор, поэтому каждая клавиша
; проверяется ровно раз. KBSCN разбирает те же ячейки автоповтора — он
; последний, чтобы не перехватить первое нажатие пробела или Enter.
Ui_Keys:
        call WC_UP
        ld a,UI_KEY_UP
        call nz,Ui_Latch
        call WC_DOWN
        ld a,UI_KEY_DOWN
        call nz,Ui_Latch
        call WC_PGUP
        ld a,UI_KEY_PGUP
        call nz,Ui_Latch
        call WC_PGDN
        ld a,UI_KEY_PGDN
        call nz,Ui_Latch
        call WC_HOME
        ld a,UI_KEY_HOME
        call nz,Ui_Latch
        call WC_END
        ld a,UI_KEY_END
        call nz,Ui_Latch
        call WC_SPACE
        ld a,UI_KEY_SPACE
        call nz,Ui_Latch
        call WC_ENTER
        jr z,.esc
        call Ui_Flush                   ; отметка пробелом — раньше Enter
        call Ui_Enter
.esc:
        call WC_ESC
        jr nz,.exit
        ld a,1                          ; сырая таблица: латинская A при любой раскладке
        call WC_KBSCN
        ret z
        or #20
        cp 'a'
        ret nz
        call Ui_Flush
        jp Ui_SelectAll
.exit:
        ld a,1
        ld (WcuExit),a
        ret

Ui_Up:
        ld a,(UiCursor)
        or a
        ret z
        dec a
        jr Ui_SetCursor

Ui_Down:
        ld a,(WcuCount)
        ld b,a
        ld a,(UiCursor)
        inc a
        cp b
        ret nc
        jr Ui_SetCursor

Ui_PageUp:
        ld a,(UiRows)
        ld b,a
        ld a,(UiCursor)
        sub b
        jr nc,Ui_SetCursor
        xor a
        jr Ui_SetCursor

Ui_PageDown:
        ld a,(WcuCount)
        or a
        ret z
        dec a
        ld c,a                          ; последняя строка
        ld a,(UiRows)
        ld b,a
        ld a,(UiCursor)
        add a,b
        jr c,.last
        cp c
        jr c,Ui_SetCursor
.last:
        ld a,c
        jr Ui_SetCursor

Ui_Home:
        xor a
        jr Ui_SetCursor

Ui_End:
        ld a,(WcuCount)
        or a
        ret z
        dec a
; A — новая строка курсора (в пределах списка). Окно сдвигается ровно
; настолько, чтобы курсор был виден.
Ui_SetCursor:
        ld hl,UiCursor
        cp (hl)
        ret z
        push af
        ld a,(hl)
        call Ui_DirtyIndex              ; прежняя строка — без курсора
        pop af
        ld (UiCursor),a
        ld b,a
        ld hl,UiTop
        cp (hl)
        jr nc,.not_above
        ld (hl),a                       ; курсор выше окна
        jp Ui_DirtyAll
.not_above:
        ld a,(UiRows)
        add a,(hl)
        ld c,a                          ; первая строка ниже окна
        ld a,b
        cp c
        jr c,.visible
        ld a,(UiRows)
        ld c,a
        ld a,b
        sub c
        inc a
        ld (hl),a                       ; курсор — на нижней строке окна
        jp Ui_DirtyAll
.visible:
        ld a,b
        jp Ui_DirtyIndex

; NZ — список можно править: проверка закончена и обновление не идёт.
Ui_CanEdit:
        ld a,(WcuBusy)
        or a
        jr nz,.no
        ld a,(WcuPhase)
        cp WCU_PH_READY
        jr nz,.no
        ld a,(WcuCount)
        or a
        ret
.no:
        xor a
        ret

; Пробел: снять или поставить отметку и перейти на строку ниже.
Ui_Space:
        call Ui_CanEdit
        ret z
        call Wcu_MapTable
        ld a,(UiCursor)
        call Wcu_EntryAddr
        inc hl                          ; E_FLAGS
        bit WCU_B_CAN,(hl)
        jr z,.next                      ; совпадающий или защищённый — не отмечается
        ld a,(hl)
        xor WCU_F_MARK
        ld (hl),a
        ld a,(UiCursor)
        call Ui_DirtyIndex
.next:
        jp Ui_Down

; A: отметить всё, что ESP пометила как исполняемое и отличающееся (.WMF,
; .$C, .SPG). Совпадающие, тексты и wc.ini не отмечаются; ручные отметки
; остаются.
Ui_SelectAll:
        call Ui_CanEdit
        ret z
        call Wcu_MapTable
        ld a,(WcuCount)
        ld b,a
        ld hl,WCU_TABLE+E_FLAGS
        ld de,WCU_ENTRY_SIZE
.row:
        bit WCU_B_AUTO,(hl)
        jr z,.skip
        set WCU_B_MARK,(hl)
.skip:
        add hl,de
        djnz .row
        jp Ui_DirtyAll

; Enter: отдать ESP номера отмеченных строк. Ничего не отмечено — строку под
; курсором, если её можно обновить.
Ui_Enter:
        call Ui_CanEdit
        ret z
        ld a,(WcuSyncPending)
        or a
        ret nz                          ; список ещё обновляется
        call Wcu_MapTable
        ld a,(WcuCount)
        ld b,a
        ld c,0
        ld hl,WCU_TABLE+E_FLAGS
        ld de,WcuApplyList
.collect:
        bit WCU_B_MARK,(hl)
        jr z,.next
        ld a,c
        ld (de),a
        inc de
.next:
        push de
        ld de,WCU_ENTRY_SIZE
        add hl,de
        pop de
        inc c
        djnz .collect
        ex de,hl
        ld de,WcuApplyList
        or a
        sbc hl,de
        ld a,l                          ; отмечено строк
        or a
        jr nz,.send
        ld a,(UiCursor)
        call Wcu_EntryAddr
        inc hl
        bit WCU_B_CAN,(hl)
        ret z
        ld a,(UiCursor)
        ld (WcuApplyList),a
        ld a,1
.send:
        ld (UiApplyCount),a
        ; Отпускания Enter не ждём: блокирующее ожидание оставило бы очередь
        ; ZiFi без чтения. Удержанный Enter повторится автоповтором, но до
        ; итога обновления он занятостью и игнорируется.
        ; Занятость и повтор списка после итога — до отправки: итог может
        ; прийти ещё внутри ожидания ответа (или вместо потерянного A6).
        ld a,1
        ld (WcuBusy),a
        ld (WcuApplied),a
        ld (WcuRefreshAfter),a
        ld hl,UiStageApply
        call Ui_SetStatus
        ld hl,WcuApplyList
        ld a,(UiApplyCount)
        ld c,a
        ld b,0
        call Wcu_Apply
        ret nc
        xor a
        ld (WcuBusy),a
        ld (WcuRefreshAfter),a          ; события APPLY, если он всё же идёт, вернут
        ld a,(WcuExit)
        or a
        ret nz                          ; Esc во время ожидания: выходим
        ld hl,UiErrorApply
        jp Ui_SetStatus

; --- описатель окна и тексты --------------------------------------------------

; Тип 1: заголовки и текст без курсора; +12/+14/+16 — верхний и нижний
; заголовки и текст. Содержимое выводится отдельно, RRESB восстанавливает фон.
WcuWindow:
        db #81                         ; тень и рамка стиля 1
        db 0
        db #FF,#FF                     ; X,Y: центр, см. Ui_Open
        db UI_W,UI_H                   ; ширина; высота уточняется в Ui_Open
        db UI_COLOR
        db 0
        dw 0
        db 0,0
        dw UiTitle
        dw UiFooter
        dw 0

UiTitle:        db #0E,9," ZiFi WC Update ",0
UiFooter:       db #0E,9," Space mark  A all  Enter update  Esc exit ",0

UiHeadText:
        db " Upd File"
        ds UI_COL_SD-($-UiHeadText),' '
        db "     SD"
        db "  GitHub"
        db " State"
        ds UI_IW-($-UiHeadText),' '
        ASSERT $-UiHeadText == UI_IW

; Слова состояний (WCU_ST_*): 8 символов и цвет.
UiStateTable:
        db "...     ",UI_COLOR          ; ещё не проверен
        db "same    ",UI_COLOR
        db "DIFFERS ",UI_COLOR_WARN
        db "NEW     ",UI_COLOR_WARN
        db "SD only ",UI_COLOR
        db "kept    ",UI_COLOR          ; wc.ini совпадает с GitHub
        db "kept    ",UI_COLOR          ; wc.ini свой: не заменяется
        db "READ ERR",UI_COLOR_BAD
        db "UPDATED ",UI_COLOR_GOOD
        db "FAILED  ",UI_COLOR_BAD
        ASSERT $-UiStateTable == 9*(WCU_ST_FAILED+1)

UiStageConfig:     db "Reading /zifi/zifi.ini",0
UiStageZifi:       db "Detecting ZiFi",0
UiStageWifi:       db "Connecting to Wi-Fi",0
UiStageStart:      db "Starting the check on ESP",0
UiStageApply:      db "Starting the update",0
UiStageStopping:   db "Stopping",0
UiErrorWord:       db "ERROR: ",0
UiErrorSd:         db "ERROR: no readable SD volume",0
UiErrorDir:        db "ERROR: /zifi directory missing",0
UiErrorIni:        db "ERROR: /zifi/zifi.ini missing",0
UiErrorConfig:     db "ERROR: invalid zifi.ini",0
UiErrorNoZifi:     db "ERROR: ZiFi not detected",0
UiErrorFirmware:   db "ERROR: ESP does not answer (no READY)",0
UiErrorStart:      db "ERROR: ESP firmware has no WC update",0
UiErrorApply:      db "ERROR: ESP did not accept the update",0
UiRestartHint:     db " - restart WC",0
UiErrorStale:      db "ERROR: list not refreshed - press Esc and run again",0
UiHuge:            db ">16M"
UI_HUGE_LEN        equ $-UiHuge

; --- рабочие данные -----------------------------------------------------------

UiColor:        db 0
UiStatusColor:  db UI_COLOR
UiStateColor:   db 0
UiRowColor:     db 0
UiRowNo:        db 0
UiRowIndex:     db 0
UiRowLine:      db 0
UiApplyCount:   db 0
UiNumber:       ds 3
UiDigits:       ds 12
UiOut:          dw 0
UiPowPtr:       dw 0
UiDigitsLeft:   db 0
UiStarted:      db 0
UiStatusBuf:    ds UI_IW
; Строка списка, полоса хода с числами, вертикальная полоса прокрутки
; (два байта на строку) и строка отказа Wi-Fi.
UI_ROWBUF_SIZE  equ UI_IW+18
RowBuf:         ds UI_ROWBUF_SIZE
        ASSERT UI_ROWBUF_SIZE >= 2*UI_MAX_ROWS
; Строка отказа Wi-Fi (Link_BuildFail) собирается здесь же: окно ещё не
; рисует список, а Ui_StatusNow переписывает её в UiStatusBuf. Префикс,
; до пяти цифр, пробел и доклад ESP с нулём.
LinkFailBuf     equ RowBuf
        ASSERT UI_ROWBUF_SIZE >= 17+5+1+PROTO_ERR_TEXT
