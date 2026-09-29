; Шкала уровня Wi-Fi в поле Status FTP- и SMB-плагинов.
;
; ESP присылает готовую строку событием EVT_WIFI_SIGNAL: "Wi-Fi [", 16 клеток
; '#'/'.', "] NNN%". Клетки '#' и процент плагин красит по уровню: от 60 %
; (-66 дБм) — зелёным, от 30 % (-78 дБм) — жёлтым, ниже — красным. Процент
; ESP считает линейно: -90 дБм — 0 %, -50 дБм — 100 %.
;
; Цвет задают коды TXTPR прямо в тексте поля (WCIF.ASM, TXTPR): два одинаковых
; кода 1..8 — яркие чернила этого цвета на фоне окна, одиночный код — обычные
; чернила; перевод строки #0D сам возвращает цвет окна. Коды места на экране
; не занимают, и их в поле не больше пяти (два перед клетками, один после них,
; два перед процентом), поэтому поле на пять байтов длиннее видимой ширины.
;
; От плагина нужны UiStatusField, UI_FIELD_STATUS (видимая ширина поля) и
; UI_STATUS_BYTES (байтов поля вместе с местом под коды).

UI_SIGNAL_GOOD_FROM equ 60              ; с этого процента сигнал хороший
UI_SIGNAL_FAIR_FROM equ 30              ; с этого — средний, ниже — плохой
UI_INK_GREEN    equ 4                   ; коды цвета TXTPR
UI_INK_YELLOW   equ 6
UI_INK_RED      equ 2
UI_INK_WINDOW   equ 7                   ; одиночный: чернила окна (#17)
UI_SIGNAL_CODES equ 5

        ASSERT UI_STATUS_BYTES >= UI_FIELD_STATUS+UI_SIGNAL_CODES

; HL — строка уровня Wi-Fi с нулём на конце.
Ui_SetSignal:
        push hl
        ; Процент — цифры после ']'.
        ld bc,0                         ; C — процент, B — ']' уже был
.scan:
        ld a,(hl)
        inc hl
        or a
        jr z,.level
        cp ']'
        jr nz,.digit
        inc b
        jr .scan
.digit:
        sub '0'
        cp 10
        jr nc,.scan
        ld e,a
        ld a,b
        or a
        jr z,.scan                      ; цифры до ']' — не процент
        ld a,c
        add a,a
        ld d,a
        add a,a
        add a,a
        add a,d
        add a,e
        ld c,a                          ; C = C*10 + цифра
        jr .scan
.level:
        ld a,c
        ld c,UI_INK_GREEN
        cp UI_SIGNAL_GOOD_FROM
        jr nc,.ink
        ld c,UI_INK_YELLOW
        cp UI_SIGNAL_FAIR_FROM
        jr nc,.ink
        ld c,UI_INK_RED
.ink:
        ld a,c
        ld (UiSignalInk),a
        call Ui_ClearStatus
        pop hl
        ; Копия с кодами цвета, каждый — не больше одного раза, так что поле не
        ; переполнит и строка другого вида. C: бит 0 — клетки начались, бит 1 —
        ; кончились, бит 2 — ']' уже был, бит 3 — процент окрашен. B — сколько
        ; видимых знаков ещё влезет.
        ld de,UiStatusField
        ld bc,UI_FIELD_STATUS*256
.copy:
        ld a,b
        or a
        ret z
        ld a,(hl)
        or a
        ret z
        cp '#'
        jr nz,.not_cell
        bit 0,c
        jr nz,.percent
        set 0,c                         ; первая клетка: цвет уровня
        call .ink2
        jr .percent
.not_cell:
        bit 0,c
        jr z,.percent
        bit 1,c
        jr nz,.percent
        set 1,c                         ; клетки кончились: чернила окна
        ld a,UI_INK_WINDOW
        ld (de),a
        inc de
.percent:
        bit 2,c
        jr z,.put
        bit 3,c
        jr nz,.put
        ld a,(hl)
        cp ' '
        jr z,.put
        set 3,c                         ; первая цифра процента: цвет уровня
        call .ink2
.put:
        ld a,(hl)
        cp ']'
        jr nz,.store
        set 2,c
.store:
        cp ' '
        jr nc,.printable
        ld a,'.'                        ; управляющие байты на экран не пускать
.printable:
        ld (de),a
        inc de
        inc hl
        dec b
        jr .copy
.ink2:
        ld a,(UiSignalInk)
        ld (de),a
        inc de
        ld (de),a
        inc de
        ret

; Поле Status целиком — пробелами, вместе с местом под коды цвета: и простой
; текст, и шкала перекрывают прежнее содержимое полностью.
Ui_ClearStatus:
        ld hl,UiStatusField
        ld de,UiStatusField+1
        ld bc,UI_STATUS_BYTES-1
        ld (hl),' '
        ldir
        ret

UiSignalInk:    db UI_INK_RED
