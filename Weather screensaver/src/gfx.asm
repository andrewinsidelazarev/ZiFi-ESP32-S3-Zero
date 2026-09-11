; Графические примитивы: заливки, панели со скруглёнными углами, иконки,
; глифы шрифтов. Все координаты — в пикселях экрана 360x288, цвета —
; индексы палитры (один байт на точку: режим 256 цветов).
;
; Как устроен экран (подробнее — video.asm). Строка экрана занимает в памяти
; 512 байт, из которых видны первые 360; одна видеостраница 16 КиБ вмещает
; 32 строки. Процедура Video_Row подключает к окну #C000 страницу нужной
; строки и возвращает в HL адрес её первой точки. Дальше точка (x, y) — это
; просто байт по адресу HL + x.
;
; Параметры передаются через ячейки памяти GfxX/GfxY/GfxW/GfxH и GfxColor, а
; не через регистры: у Z80 регистров мало, а вызовы с именованными ячейками
; начинающему читать проще («что куда положили» видно по именам).

; --- заливка прямоугольника ----------------------------------------------------------
; Вход: GfxX, GfxY — левый верхний угол; GfxW, GfxH — размер; GfxColor — цвет.
; Рисует строку за строкой: для каждой строки находит её адрес и заполняет
; GfxW байтов подряд одним цветом.
Gfx_FillRect:
        ld hl,(GfxH)
        ld a,h
        or l                            ; H or L = 0 только при HL = 0
        ret z                           ; нулевая высота — рисовать нечего
        ld (GfxRows),hl                 ; счётчик оставшихся строк
        ld hl,(GfxY)
        ld (GfxRow),hl                  ; номер текущей строки экрана
.row:
        ld hl,(GfxRow)
        call Video_Row                  ; HL — начало строки в окне #C000
        ld de,(GfxX)
        add hl,de                       ; HL — адрес точки (GfxX, строка)
        ld bc,(GfxW)
        ld a,(GfxColor)
        call Gfx_FillBytes              ; BC байтов цветом A
        ld hl,(GfxRow)
        inc hl                          ; следующая строка экрана
        ld (GfxRow),hl
        ld hl,(GfxRows)
        dec hl                          ; строк осталось на одну меньше
        ld (GfxRows),hl
        ld a,h
        or l
        jr nz,.row                      ; пока счётчик не дошёл до нуля
        ret

; Заполнить BC байтов (BC >= 1) с адреса HL значением A.
; Приём с LDIR: первый байт пишем сами, затем LDIR копирует байт по адресу
; HL в HL+1, потом HL+1 в HL+2 и так далее — значение «размножается» вправо.
Gfx_FillBytes:
        ld (hl),a                       ; первый байт
        dec bc                          ; один уже записан
        ld a,b
        or c
        ret z                           ; был ровно один байт
        ld d,h
        ld e,l
        inc de                          ; DE = HL + 1 — куда копировать
        ldir                            ; копирование «сам в себя» со сдвигом 1
        ret

; Горизонтальная линия: GfxX, GfxY, GfxW, GfxColor.
; Это прямоугольник высотой в одну строку.
Gfx_HLine:
        ld hl,1
        ld (GfxH),hl
        jr Gfx_FillRect

; Вертикальная линия: GfxX, GfxY, GfxH, GfxColor.
; Это прямоугольник шириной в одну точку.
Gfx_VLine:
        ld hl,1
        ld (GfxW),hl
        jr Gfx_FillRect

; Поставить точку: HL = x, DE = y, A = цвет.
Gfx_PutPixel:
        push hl                         ; x понадобится после Video_Row
        ld (GfxColor),a                 ; цвет — в ячейку: A испортится
        ex de,hl                        ; HL = y для Video_Row
        call Video_Row                  ; HL — начало строки y
        pop de                          ; DE = x
        add hl,de                       ; адрес точки
        ld a,(GfxColor)
        ld (hl),a
        ret

; --- фон: вертикальный градиент из 16 полос по 18 строк ----------------------------
; Цвета 0..15 палитры — плавный переход от верхнего цвета фона к нижнему.
; Каждая строка экрана заливается цветом своей полосы.
Gfx_Background:
        ld hl,0
        ld (GfxRow),hl                  ; начинаем с верхней строки
.row:
        ld hl,(GfxRow)
        call Gfx_BgColor                ; A — цвет полосы, куда попала строка
        ld (GfxColor),a
        ld hl,(GfxRow)
        call Video_Row
        ld bc,SCREEN_W                  ; вся видимая ширина строки
        ld a,(GfxColor)
        call Gfx_FillBytes
        ld hl,(GfxRow)
        inc hl
        ld (GfxRow),hl
        ld de,SCREEN_H
        or a                            ; CF=0: SBC вычитает ещё и флаг переноса
        sbc hl,de                       ; HL - 288: ноль — все строки готовы
        jr nz,.row
        ret

; Цвет фона для строки HL: PAL_BG0 + y/18. Выход: A. Портит HL, DE.
; Деления в Z80 нет, поэтому делим вычитанием: сколько раз из y удалось
; вычесть 18, таков номер полосы.
Gfx_BgColor:
        ld de,SCREEN_H/16               ; высота одной полосы градиента
        xor a                           ; A = 0 и заодно CF = 0
.divide:
        sbc hl,de                       ; при входе CF=0
        jr c,.done                      ; стало меньше нуля — остаток найден
        inc a                           ; ещё одна целая полоса
        or a                            ; CF=0 для следующего вычитания
        jr .divide
.done:
        add a,PAL_BG0
        ret

; --- панель с рамкой и скруглёнными углами ---------------------------------------
; Вход: GfxX, GfxY, GfxW, GfxH; GfxColor — заливка; GfxBorder — рамка.
; Сначала рисуется обычный прямоугольник с рамкой в 1 точку. Потом углы
; скругляются по маске CornerMask (размер CORNER_RADIUS x CORNER_RADIUS,
; левый верхний угол): 0 — точка снаружи панели (закрасить фоном), 1 — точка
; рамки, 2 — заливка, ничего не менять. Маска отражается на все четыре угла.
Gfx_Panel:
        ; координаты панели нужны и после вызовов, которые меняют GfxX..GfxH
        ld hl,(GfxX)
        ld (PanX),hl
        ld hl,(GfxY)
        ld (PanY),hl
        ld hl,(GfxW)
        ld (PanW),hl
        ld hl,(GfxH)
        ld (PanH),hl
        ld a,(GfxColor)
        ld (PanFill),a
        call Gfx_FillRect               ; заливка всей панели
        ld a,(GfxBorder)
        ld (GfxColor),a
        call Gfx_HLine                  ; верхняя граница (GfxY не менялся)
        ld hl,(PanY)
        ld de,(PanH)
        add hl,de
        dec hl                          ; y нижней строки = Y + H - 1
        ld (GfxY),hl
        call Gfx_HLine                  ; нижняя граница
        ld hl,(PanY)
        ld (GfxY),hl
        ld hl,(PanH)
        ld (GfxH),hl
        call Gfx_VLine                  ; левая граница
        ld hl,(PanX)
        ld de,(PanW)
        add hl,de
        dec hl                          ; x правого столбца = X + W - 1
        ld (GfxX),hl
        call Gfx_VLine                  ; правая граница
        ; углы: двойной цикл по клеткам маски, cy — строка, cx — столбец
        xor a
        ld (CornerCY),a
.corner_row:
        xor a
        ld (CornerCX),a
.corner_col:
        ; номер клетки маски = cy * RADIUS + cx; умножение — сложением
        ld a,(CornerCY)
        ld l,a
        ld h,0
        ld e,l                          ; индекс = cy*RADIUS + cx
        ld d,h
        ld b,CORNER_RADIUS-1            ; HL = cy, прибавить cy ещё R-1 раз
.mul:
        add hl,de
        djnz .mul                       ; DJNZ: B = B - 1, повтор, пока B <> 0
        ld a,(CornerCX)
        ld e,a
        add hl,de
        ld de,CornerMask
        add hl,de                       ; HL — адрес клетки маски
        ld a,(hl)
        cp 2
        jr z,.next_col                  ; заливка уже на месте
        ld (CornerCell),a
        ; четыре отражённые точки: слева/справа и сверху/снизу
        ld a,(CornerCX)
        ld hl,(PanX)
        call Gfx_AddByte                ; x = PanX + cx
        ld (CornerX0),hl
        ld hl,(PanX)
        ld de,(PanW)
        add hl,de
        dec hl
        ld a,(CornerCX)
        call Gfx_SubByte                ; x = PanX + PanW - 1 - cx
        ld (CornerX1),hl
        ld a,(CornerCY)
        ld hl,(PanY)
        call Gfx_AddByte                ; y = PanY + cy
        ld (CornerY0),hl
        ld hl,(PanY)
        ld de,(PanH)
        add hl,de
        dec hl
        ld a,(CornerCY)
        call Gfx_SubByte                ; y = PanY + PanH - 1 - cy
        ld (CornerY1),hl
        ld hl,(CornerX0)
        ld de,(CornerY0)
        call Gfx_CornerPixel            ; левый верхний угол
        ld hl,(CornerX1)
        ld de,(CornerY0)
        call Gfx_CornerPixel            ; правый верхний
        ld hl,(CornerX0)
        ld de,(CornerY1)
        call Gfx_CornerPixel            ; левый нижний
        ld hl,(CornerX1)
        ld de,(CornerY1)
        call Gfx_CornerPixel            ; правый нижний
.next_col:
        ld a,(CornerCX)
        inc a
        ld (CornerCX),a
        cp CORNER_RADIUS
        jp c,.corner_col                ; CF=1: cx < RADIUS — следующий столбец
        ld a,(CornerCY)
        inc a
        ld (CornerCY),a
        cp CORNER_RADIUS
        jp c,.corner_row                ; следующая строка маски
        ret

; Точка угла: HL = x, DE = y; цвет по CornerCell (0 — снаружи, 1 — рамка).
; Снаружи панели — градиент фона (GfxOutside=#FF) либо заданный цвет, когда
; панель лежит внутри другой панели (лента календаря).
Gfx_CornerPixel:
        push hl
        push de
        ld a,(CornerCell)
        or a
        jr nz,.border                   ; 1 — точка рамки
        ld a,(GfxOutside)
        cp #FF
        jr nz,.draw                     ; задан цвет снаружи
        ex de,hl                        ; HL = y
        call Gfx_BgColor                ; A — цвет градиента этой строки
        jr .draw
.border:
        ld a,(GfxBorder)
.draw:
        pop de                          ; DE = y
        pop hl                          ; HL = x
        jp Gfx_PutPixel                 ; JP вместо CALL+RET: вернёмся прямо к вызвавшему

; HL = HL + A (беззнаково).
; ADD A,L складывает младшие байты; перенос (CF) добавляется к старшему.
Gfx_AddByte:
        add a,l
        ld l,a
        ret nc                          ; переноса нет — H не меняется
        inc h
        ret

; HL = HL - A (беззнаково). Портит DE.
Gfx_SubByte:
        ld e,a
        ld d,0                          ; DE = A
        or a                            ; CF=0 перед SBC
        sbc hl,de
        ret

; --- иконка: копирование байтов из страницы данных -----------------------------------
; Вход: HL — адрес записи таблицы иконок (db страница, dw смещение);
;       A — размер стороны; GfxX, GfxY — куда.
; Иконка хранится готовыми индексами палитры, строка за строкой, поэтому
; каждая её строка переносится на экран одной командой LDIR.
Gfx_Icon:
        ld (GfxIconSize),a
        ld a,(hl)                       ; номер страницы данных
        inc hl
        ld e,(hl)
        inc hl
        ld d,(hl)                       ; DE — смещение в странице (= адрес в #0000)
        ld (GfxSrc),de
        call Data_Page                  ; страница с иконкой — в окно #0000
        ld a,(GfxIconSize)
        ld (GfxRows),a                  ; строк столько же, сколько точек в строке
        xor a
        ld (GfxRows+1),a
        ld hl,(GfxY)
        ld (GfxRow),hl
.row:
        ld hl,(GfxRow)
        call Video_Row
        ld de,(GfxX)
        add hl,de
        ex de,hl                        ; DE — приёмник в видеостранице
        ld hl,(GfxSrc)                  ; HL — источник в странице данных
        ld a,(GfxIconSize)
        ld c,a
        ld b,0                          ; BC — длина строки иконки
        ldir
        ld (GfxSrc),hl                  ; следующая строка иконки
        ld hl,(GfxRow)
        inc hl
        ld (GfxRow),hl
        ld hl,(GfxRows)
        dec hl
        ld (GfxRows),hl
        ld a,h
        or l
        jr nz,.row
        ret

; --- глиф: 2 бита на пиксель -------------------------------------------------------
; Вход: HL — запись глифа в подключённой странице данных
;       (advance, width, height, top, left, затем строки битмапа);
;       TxtX, TxtY — перо и верх строки; TxtSet — набор цветов текста.
; Каждая точка глифа — 2 бита: уровень 0 прозрачен (фон не трогаем), уровни
; 1..3 — всё более яркие смеси цвета букв с цветом подложки. В палитре на
; каждый набор (например, «белый на панели») отведено три цвета подряд:
; PAL_TEXT0+set*3 .. +2. Так без вычислений можно сгладить края букв.
; Сейчас шрифты растрируются чёткими (tools/design.py): в них только уровни
; 0 и 3, то есть точка либо фон, либо цвет букв — полутона выглядели размыто.
; В байте четыре точки, старшие биты — левая точка.
; Выход: TxtX сдвинут на advance глифа (ширину шага символа).
Gfx_Glyph:
        ; заголовок глифа — пять байтов
        ld a,(hl)
        ld (GlyphAdv),a                 ; шаг пера
        inc hl
        ld a,(hl)
        ld (GlyphW),a                   ; ширина рисунка в точках
        inc hl
        ld a,(hl)
        ld (GlyphH),a                   ; высота рисунка в строках
        inc hl
        ld a,(hl)
        ld (GlyphTop),a                 ; отступ рисунка от верха строки
        inc hl
        ld a,(hl)                       ; left — знаковый
        inc hl
        ld (GfxSrc),hl                  ; начало точек рисунка
        ; x0 = TxtX + left; left бывает отрицательным (-128..127), поэтому
        ; его надо «расширить знаком» до 16 бит: D = #FF для отрицательных
        ld hl,(TxtX)
        ld e,a
        ld d,0
        or a
        jp p,.left_positive             ; флаг S=0 — число неотрицательное
        ld d,#FF                        ; расширение знака
.left_positive:
        add hl,de
        ld (GlyphX),hl
        ; y0 = TxtY + top
        ld a,(GlyphTop)
        ld hl,(TxtY)
        call Gfx_AddByte
        ld (GfxRow),hl
        ; ширина в байтах = (w+3)/4: по 4 точки в байте, с округлением вверх
        ld a,(GlyphW)
        add a,3
        srl a                           ; SRL — сдвиг вправо = деление на 2
        srl a
        ld (GlyphStride),a
        ; базовый цвет: PAL_TEXT0 + set*3 - 1, чтобы цвет = база + уровень
        ld a,(TxtSet)
        ld b,a
        add a,a                         ; set*2
        add a,b                         ; set*3
        add a,PAL_TEXT0-1
        ld (GlyphBase),a
        ld a,(GlyphH)
        or a
        jr z,.advance                   ; пробел: только сдвиг пера
        ld (GfxRows),a
.row:
        ld hl,(GfxRow)
        call Video_Row
        ld de,(GlyphX)
        add hl,de
        ex de,hl                        ; DE — приёмник
        ld hl,(GfxSrc)
        ld a,(GlyphStride)
        ld c,a                          ; C — байтов в строке
        ld a,(GlyphBase)
        ld b,a                          ; B — базовый цвет
.byte:
        ld a,(hl)
        inc hl
        ld (GlyphByte),a
        ; четыре пикселя старшими битами вперёд: каждый раз сдвигаем байт
        ; так, чтобы нужная пара битов оказалась в самых младших разрядах
        rlca                            ; RLCA — циклический сдвиг влево:
        rlca                            ; биты 7..6 ушли в 1..0 — первая точка
        call Gfx_GlyphPixel
        ld a,(GlyphByte)
        rrca                            ; RRCA — циклический сдвиг вправо:
        rrca                            ; биты 5..4 за четыре шага
        rrca                            ; оказываются в 1..0 — вторая точка
        rrca
        call Gfx_GlyphPixel
        ld a,(GlyphByte)
        rrca                            ; биты 3..2 -> 1..0 — третья точка
        rrca
        call Gfx_GlyphPixel
        ld a,(GlyphByte)                ; биты 1..0 — четвёртая точка
        call Gfx_GlyphPixel
        dec c
        jr nz,.byte                     ; следующий байт строки глифа
        ld (GfxSrc),hl
        ld hl,(GfxRow)
        inc hl
        ld (GfxRow),hl
        ld a,(GfxRows)
        dec a
        ld (GfxRows),a
        jr nz,.row                      ; следующая строка глифа
.advance:
        ld a,(GlyphAdv)
        ld hl,(TxtX)
        call Gfx_AddByte                ; перо — к началу следующего символа
        ld (TxtX),hl
        ret

; Прямоугольник, который займёт глиф HL при пере TxtX и строке TxtY:
; результат в GfxX, GfxY, GfxW, GfxH (для стирания одного глифа).
; Порядок полей заголовка тот же, что в Gfx_Glyph: шаг, ширина, высота,
; отступ сверху, отступ слева.
Gfx_GlyphBox:
        inc hl                          ; шаг пропускаем
        ld a,(hl)                       ; ширина
        ld (GfxW),a
        inc hl
        ld a,(hl)                       ; высота
        ld (GfxH),a
        xor a
        ld (GfxW+1),a                   ; старшие байты размеров — нули
        ld (GfxH+1),a
        inc hl
        ld a,(hl)                       ; отступ сверху
        push hl
        ld hl,(TxtY)
        call Gfx_AddByte
        ld (GfxY),hl
        pop hl
        inc hl
        ld e,(hl)                       ; отступ слева, знаковый
        ld d,0
        bit 7,e                         ; бит 7 — знак числа
        jr z,.left_positive
        ld d,#FF                        ; расширение знака до 16 бит
.left_positive:
        ld hl,(TxtX)
        add hl,de
        ld (GfxX),hl
        ret

; Один пиксель глифа: младшие два бита A — уровень; DE — адрес, B — база.
; Уровень 0 — точку пропускаем (под ней остаётся фон).
Gfx_GlyphPixel:
        and 3                           ; оставить только два младших бита
        jr z,.skip
        add a,b                         ; цвет = база + уровень
        ld (de),a
.skip:
        inc de                          ; следующая точка экрана
        ret

; --- переменные примитивов -----------------------------------------------------------
GfxX:           dw 0                    ; левый край
GfxY:           dw 0                    ; верхний край
GfxW:           dw 0                    ; ширина
GfxH:           dw 0                    ; высота
GfxColor:       db 0                    ; цвет заливки или линии
GfxBorder:      db 0                    ; цвет рамки панели
GfxOutside:     db #FF                  ; цвет снаружи углов панели, #FF — градиент
GfxRow:         dw 0                    ; текущая строка
GfxRows:        dw 0                    ; сколько строк осталось
GfxSrc:         dw 0                    ; источник в странице данных
GfxIconSize:    db 0                    ; сторона иконки в точках
PanX:           dw 0                    ; копия координат панели для углов
PanY:           dw 0
PanW:           dw 0
PanH:           dw 0
PanFill:        db 0
CornerCX:       db 0                    ; столбец клетки маски угла
CornerCY:       db 0                    ; строка клетки маски угла
CornerCell:     db 0                    ; значение клетки: 0, 1 или 2
CornerX0:       dw 0                    ; x левой и правой точки угла
CornerX1:       dw 0
CornerY0:       dw 0                    ; y верхней и нижней точки угла
CornerY1:       dw 0
GlyphAdv:       db 0                    ; поля заголовка текущего глифа
GlyphW:         db 0
GlyphH:         db 0
GlyphTop:       db 0
GlyphStride:    db 0                    ; байтов в строке рисунка
GlyphBase:      db 0                    ; цвет для уровня 0 (уровни 1..3 — выше)
GlyphByte:      db 0                    ; текущий байт рисунка
GlyphX:         dw 0                    ; x левой точки рисунка
