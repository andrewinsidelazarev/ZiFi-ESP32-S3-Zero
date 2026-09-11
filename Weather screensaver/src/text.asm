; Вывод строк пропорциональным шрифтом в видеостраницы TS-Conf.
; Сборка строк с числами (Fmt_..., буфер TextBuf) — общая с заставкой для
; VDAC2 и лежит в ../shared/weather/fmt.asm.
;
; «Пропорциональный» значит, что символы разной ширины: «ш» шире «i». Поэтому
; положение следующего символа хранится в «пере» TxtX: после каждого символа
; перо сдвигается на его шаг (advance).
;
; Шрифты лежат в странице данных FONT_PAGE (см. assets.inc). Заголовок шрифта:
;   +0 высота строки, +1 ascent, +2 первый код, +3 последний код,
;   +4..+5 резерв, +6 смещение таблицы глифов (u16 на каждый код, 0 — нет).
; Таблица по коду символа даёт адрес его записи (глифа) — см. Gfx_Glyph.
; Строки — в кодировке CP866 с нулём в конце; #F9 — многоточие, #FA — маркер.
; Параметры вывода: TxtX/TxtY (перо и верх строки), TxtFont (смещение
; заголовка шрифта), TxtSet (набор цветов 0..7), TxtSpacing (добавка к шагу).

; Нарисовать строку HL. Выход: TxtX — перо после последнего символа.
; Символ, который закончился бы правее TxtRight, не рисуется, и на нём вывод
; строки прекращается (TxtRight = #FFFF — ограничения нет).
Text_Draw:
        ld a,FONT_PAGE
        call Data_Page                  ; шрифты — в окно #0000
.next:
        ld a,(hl)                       ; очередной символ
        or a
        ret z                           ; ноль — конец строки
        inc hl
        push hl                         ; указатель строки портят вызовы ниже
        call Text_GlyphPtr              ; HL — запись глифа
        jr c,.skip                      ; символа нет в шрифте — пропустить
        push hl
        ld a,(hl)                       ; шаг символа
        ld hl,(TxtX)
        call Gfx_AddByte                ; где окажется перо после символа
        ld de,(TxtRight)
        or a
        sbc hl,de                       ; перо - TxtRight
        pop hl
        jr z,.fits                      ; ровно до края — помещается
        jr nc,.clip                     ; правее края — дальше не рисуем
.fits:
        call Gfx_Glyph                  ; нарисовать и сдвинуть перо
        ld a,(TxtSpacing)
        ld hl,(TxtX)
        call Gfx_AddByte                ; дополнительный промежуток (разрядка)
        ld (TxtX),hl
.skip:
        pop hl
        jr .next
.clip:
        pop hl                          ; снять указатель строки со стека
        ret

; Ширина строки HL в пикселях. Выход: HL. Ничего не рисует.
; Складывает шаги всех символов (плюс разрядку TxtSpacing) в DE.
Text_Width:
        ld a,FONT_PAGE
        call Data_Page
        ld de,0                         ; накопленная ширина
.next:
        ld a,(hl)
        or a
        jr z,.done
        inc hl
        push hl
        push de
        call Text_GlyphPtr
        pop de
        jr c,.skip
        ld a,(hl)                       ; advance глифа
        add a,e                         ; DE = DE + шаг (8 бит к 16 битам)
        ld e,a
        jr nc,.no_carry
        inc d
.no_carry:
        ld a,(TxtSpacing)
        add a,e
        ld e,a
        jr nc,.skip
        inc d
.skip:
        pop hl
        jr .next
.done:
        ex de,hl                        ; результат — в HL
        ret

; Адрес глифа для кода A в шрифте TxtFont. Выход: HL и CF=0, либо CF=1,
; если кода нет в шрифте (символ пропускается).
Text_GlyphPtr:
        ld hl,(TxtFont)
        inc hl
        inc hl                          ; HL -> первый код шрифта
        cp (hl)                         ; первый код
        jr c,.missing                   ; A меньше первого кода
        ld c,(hl)
        inc hl
        cp (hl)                         ; последний код
        jr z,.in_range
        jr nc,.missing                  ; A больше последнего кода
.in_range:
        sub c                           ; индекс в таблице
        ld e,a
        ld d,0
        ld hl,(TxtFont)
        ld bc,6
        add hl,bc                       ; HL -> поле «смещение таблицы»
        ld a,(hl)
        inc hl
        ld h,(hl)
        ld l,a                          ; HL — таблица
        add hl,de
        add hl,de                       ; два байта на символ
        ld a,(hl)
        inc hl
        ld h,(hl)
        ld l,a                          ; HL — адрес записи глифа
        or h
        jr z,.missing                   ; нулевое смещение — глифа нет
        or a                            ; CF=0: глиф найден
        ret
.missing:
        scf                             ; SCF — установить CF=1
        ret

; Нарисовать строку HL, прижав её правым краем к TxtX.
; Начало строки = TxtX - ширина строки.
Text_DrawRight:
        push hl
        call Text_Width
        ex de,hl                        ; DE — ширина
        ld hl,(TxtX)
        or a
        sbc hl,de
        ld (TxtX),hl
        pop hl
        jp Text_Draw

; Нарисовать строку HL по центру отрезка шириной DE, начинающегося в TxtX.
; Центрируются видимые точки, а не сумма шагов: у цифр одинаковый шаг, и у
; узкой «1» слева пустое поле — по шагам «10» съезжало вправо от центра
; кружка календаря.
; Перо = TxtX + (ширина - левый - правый) / 2: тогда середина видимых
; точек (левый + правый) / 2 совпадает с серединой отрезка.
Text_DrawCentered:
        push hl
        push de
        call Text_InkBounds             ; DE — левый край точек, HL — правый
        add hl,de                       ; левый + правый
        ex de,hl
        pop hl                          ; HL — ширина отрезка
        or a
        sbc hl,de                       ; ширина - левый - правый (со знаком)
        sra h                           ; SRA сдвигает вправо, сохраняя знак,
        rr l                            ; RR подхватывает выпавший бит: /2
        ld de,(TxtX)
        add hl,de
        ld (TxtX),hl
        pop hl
        jp Text_Draw

; Границы видимых точек строки HL относительно начала пера (с TxtSpacing).
; Выход: DE — первый столбец с точками, HL — столбец за последним.
; Пустая строка или одни пробелы дают 0 и 0.
; Для каждого глифа: левый край = перо + отступ слева, правый = левый +
; ширина рисунка. Левый берём у первого видимого глифа, правый — наибольший.
Text_InkBounds:
        ld (InkStr),hl
        ld a,FONT_PAGE
        call Data_Page
        ld hl,0
        ld (InkPen),hl
        ld (InkLeft),hl
        ld (InkRight),hl
        ld a,1
        ld (InkFirst),a                 ; 1 — видимых точек ещё не было
.next:
        ld hl,(InkStr)
        ld a,(hl)
        or a
        jr z,.done
        inc hl
        ld (InkStr),hl
        call Text_GlyphPtr
        jr c,.next                      ; символа нет в шрифте
        ld a,(hl)                       ; шаг
        ld (InkAdv),a
        inc hl
        ld a,(hl)                       ; ширина рисунка
        or a
        jr z,.advance                   ; пробел: только шаг
        ld (InkW),a
        inc hl
        inc hl
        inc hl                          ; пропустить высоту и отступ сверху
        ld e,(hl)                       ; отступ слева, знаковый
        ld d,0
        bit 7,e
        jr z,.left_positive
        ld d,#FF                        ; расширение знака до 16 бит
.left_positive:
        ld hl,(InkPen)
        add hl,de                       ; перо + отступ — левый край глифа
        ld a,(InkFirst)
        or a
        jr z,.have_left                 ; левый край уже известен
        ld (InkLeft),hl
        xor a
        ld (InkFirst),a
.have_left:
        ld a,(InkW)
        call Gfx_AddByte                ; правый край глифа
        ld de,(InkRight)
        push hl
        or a
        sbc hl,de                       ; сравнение: HL - DE, CF=1 если HL < DE
        pop hl
        jr c,.advance                   ; левее уже найденного края
        ld (InkRight),hl
.advance:
        ld a,(InkAdv)
        ld hl,(InkPen)
        call Gfx_AddByte
        ld a,(TxtSpacing)
        call Gfx_AddByte
        ld (InkPen),hl
        jr .next
.done:
        ld de,(InkLeft)
        ld hl,(InkRight)
        ret

TxtX:           dw 0                    ; перо: x следующего символа
TxtY:           dw 0                    ; верх строки текста
TxtFont:        dw 0                    ; адрес заголовка шрифта в странице
TxtSet:         db 0                    ; набор цветов (TS_...)
TxtSpacing:     db 0                    ; разрядка: добавка к шагу символа
TxtRight:       dw #FFFF                ; правее этого x символы не рисуются
InkStr:         dw 0                    ; переменные Text_InkBounds
InkPen:         dw 0
InkLeft:        dw 0
InkRight:       dw 0
InkFirst:       db 0
InkAdv:         db 0
InkW:           db 0
