; Числа и строки для экрана заставок погоды: сборка строки в буфер TextBuf.
;
; Общее для обеих заставок (TS-Conf и VDAC2): как выглядят числа — «09:05»,
; «-12°», «23.9», — не зависит от того, чем потом рисуются буквы.
;
; Строка собирается по кусочкам: Fmt_Begin начинает строку, Fmt_Char/Fmt_Str/
; Fmt_U16 и другие дописывают, Fmt_End ставит завершающий ноль.
; Указатель записи — FmtPtr; буфер TextBuf на 64 байта.
Fmt_Begin:
        ld hl,TextBuf
        ld (FmtPtr),hl
        ret

; Закончить строку. Выход: HL = TextBuf — готовая строка.
Fmt_End:
        ld hl,(FmtPtr)
        ld (hl),0
        ld hl,TextBuf
        ret

; Дописать символ A. Сохраняет HL.
Fmt_Char:
        push hl
        ld hl,(FmtPtr)
        ld (hl),a
        inc hl
        ld (FmtPtr),hl
        pop hl
        ret

; Дописать строку HL (без завершающего нуля).
Fmt_Str:
        ld a,(hl)
        or a
        ret z
        call Fmt_Char
        inc hl
        jr Fmt_Str

; Дописать число HL без ведущих нулей (0 -> «0»).
; Цифры получаем вычитанием: сколько раз из числа вычитается 10000 — первая
; цифра, потом 1000, 100, 10; остаток — последняя цифра.
Fmt_U16:
        xor a
        ld (FmtLeading),a               ; пока ни одной цифры не напечатано
        ld de,10000
        call .digit
        ld de,1000
        call .digit
        ld de,100
        call .digit
        ld de,10
        call .digit
        ld a,l
        add a,'0'                       ; остаток 0..9 — в символ
        ld (FmtLeading),a               ; последняя цифра выводится всегда
        jp Fmt_Char
.digit:
        ld a,'0'-1                      ; счётчик вычитаний сразу в символах
.count:
        inc a
        or a                            ; CF=0 перед SBC
        sbc hl,de
        jr nc,.count                    ; не ушли в минус — вычитаем ещё
        add hl,de                       ; одно вычитание было лишним — вернуть
        cp '0'
        jr nz,.emit                     ; цифра не ноль — печатать
        ld c,a
        ld a,(FmtLeading)
        or a
        ld a,c
        ret z                           ; ведущий ноль не печатаем
.emit:
        ld (FmtLeading),a               ; дальше нули уже значимы
        jp Fmt_Char

; Дописать байт A как две цифры с ведущим нулём (часы, минуты).
Fmt_2d:
        ld c,'0'                        ; C — цифра десятков
.tens:
        cp 10
        jr c,.ones                      ; меньше 10 — десятки посчитаны
        sub 10
        inc c
        jr .tens
.ones:
        push af                         ; A — единицы
        ld a,c
        call Fmt_Char
        pop af
        add a,'0'
        jp Fmt_Char

; Дописать температуру: A — знаковые градусы; результат «-12°».
Fmt_Temp:
        or a
        jp p,.positive                  ; S=0 — ноль или больше
        neg                             ; NEG: A = -A, модуль числа
        push af
        ld a,'-'
        call Fmt_Char
        pop af
.positive:
        ld l,a
        ld h,0
        call Fmt_U16
        ld a,#F8                        ; знак градуса в CP866
        jp Fmt_Char

; Дописать HL как число с одним десятичным знаком (значение x10):
; 239 -> «23.9», 0 -> «0».
; Деление на 10 «столбиком» в двоичной системе: 16 раз сдвигаем делимое
; влево; выдвинутые биты копятся в A (остаток); как только A >= 10, вычитаем
; 10 и записываем 1 в освободившийся младший бит HL (бит частного). После 16
; шагов в HL — частное, в A — остаток, то есть цифра после точки.
Fmt_X10:
        ld a,h
        or l
        jr nz,.nonzero
        ld a,'0'                        ; ровно ноль печатается без «.0»
        jp Fmt_Char
.nonzero:
        xor a                           ; целая часть = HL / 10, остаток в A
        ld b,16
.divide:
        add hl,hl                       ; сдвиг делимого: старший бит в CF
        rla                             ; ...и в накопитель остатка
        cp 10
        jr c,.no_sub
        sub 10
        inc l                           ; бит частного
.no_sub:
        djnz .divide
        push af                         ; остаток — цифра после точки
        call Fmt_U16                    ; целая часть
        ld a,'.'
        call Fmt_Char
        pop af
        add a,'0'
        jp Fmt_Char

; Элемент A таблицы указателей HL. Выход: HL — строка.
; Таблица — подряд идущие адреса по 2 байта, поэтому смещение = A * 2.
Fmt_TableEntry:
        ld e,a
        ld d,0
        add hl,de
        add hl,de
        ld a,(hl)                       ; младший байт адреса
        inc hl
        ld h,(hl)                       ; старший байт
        ld l,a
        ret

FmtPtr:         dw 0                    ; куда писать следующий символ
FmtLeading:     db 0                    ; не ноль — цифры уже печатались
TextBuf:        ds 64                   ; строка, собираемая Fmt_...
