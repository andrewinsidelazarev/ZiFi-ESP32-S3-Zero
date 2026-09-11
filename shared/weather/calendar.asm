; Календарная арифметика: день недели и переход на соседние даты.
; Даты хранятся как год (16 бит), месяц 1..12, число 1..31.
; Процедуры работают с «рабочей» датой WorkY/WorkM/WorkD, чтобы календарь мог
; ходить по дням недели, не трогая показания часов (RtcYear/RtcMonth/RtcDay).

; День недели даты WorkY/WorkM/WorkD по формуле Зеллера.
; Выход: A = 0 понедельник .. 6 воскресенье. Совпадает с refscreen.weekday.
;
; Формула: h = (q + 13*(m+1)/5 + K + K/4 + J/4 + 5*J) mod 7, где q — число,
; m — месяц (март = 3 ... февраль = 14), K — год mod 100, J — год / 100;
; h = 0 означает субботу. Январь и февраль считаются 13-м и 14-м месяцами
; прошлого года — тогда високосный день оказывается в конце «года».
Cal_Weekday:
        ld hl,(WorkY)
        ld a,(WorkM)
        cp 3
        jr nc,.month_ok
        add a,12                        ; январь и февраль считаются 13 и 14
        dec hl                          ; ...предыдущего года
.month_ok:
        ld (CalM),a
        ; J = год / 100, K = год mod 100: делим вычитанием
        ld de,100
        ld b,0                          ; B — сколько раз вычли 100
.century:
        or a
        sbc hl,de
        jr c,.century_done
        inc b
        jr .century
.century_done:
        add hl,de                       ; HL = K
        ld a,b
        ld (CalJ),a
        ld a,l
        ld (CalK),a
        ; сумма = число + (13*(m+1))/5 + K + K/4 + J/4 + 5*J
        ld a,(WorkD)
        ld l,a
        ld h,0                          ; HL — накопитель
        ld a,(CalM)
        inc a
        ld c,a                          ; C = m+1
        add a,a
        add a,a
        add a,a
        add a,a                         ; 16*(m+1)
        sub c
        sub c
        sub c                           ; 13*(m+1), не больше 195
        ld c,-1                         ; частное от деления на 5
.div5:
        inc c
        sub 5
        jr nc,.div5                     ; вычитаем 5, пока не ушли в минус
        ld a,c                          ; A = (13*(m+1))/5
        call Cal_AddA
        ld a,(CalK)
        call Cal_AddA                   ; + K
        ld a,(CalK)
        srl a
        srl a                           ; K/4
        call Cal_AddA
        ld a,(CalJ)
        srl a
        srl a                           ; J/4
        call Cal_AddA
        ld a,(CalJ)
        ld c,a
        add a,a
        add a,a
        add a,c                         ; 5*J
        call Cal_AddA
        ; (сумма + 5) mod 7: сдвиг делает понедельник нулём
        ld a,5
        call Cal_AddA
        ld de,7
.mod7:
        or a
        sbc hl,de
        jr nc,.mod7
        add hl,de                       ; остаток от деления на 7
        ld a,l
        ret

; HL = HL + A.
Cal_AddA:
        add a,l
        ld l,a
        ret nc
        inc h
        ret

; Число дней в месяце A года HL. Выход: A.
Cal_DaysInMonth:
        cp 2
        jr z,.february
        cp 4
        jr z,.thirty                    ; апрель
        cp 6
        jr z,.thirty                    ; июнь
        cp 9
        jr z,.thirty                    ; сентябрь
        cp 11
        jr z,.thirty                    ; ноябрь
        ld a,31
        ret
.thirty:
        ld a,30
        ret
.february:
        ; високосный: делится на 4 и (не делится на 100 или делится на 400)
        ld a,l
        and 3                           ; делимость на 4 — два младших бита
        jr nz,.common
        push hl
        ld de,100
.mod100:
        or a
        sbc hl,de
        jr nc,.mod100
        add hl,de                       ; HL = год mod 100
        ld a,h
        or l
        pop hl
        jr nz,.leap                     ; не кратен 100 — високосный
        ld de,400
.mod400:
        or a
        sbc hl,de
        jr nc,.mod400
        add hl,de                       ; HL = год mod 400
        ld a,h
        or l
        jr z,.leap
.common:
        ld a,28
        ret
.leap:
        ld a,29
        ret

; Перевести WorkY/WorkM/WorkD на день назад.
Cal_PrevDay:
        ld a,(WorkD)
        dec a
        jr z,.prev_month                ; было 1-е число
        ld (WorkD),a
        ret
.prev_month:
        ld a,(WorkM)
        dec a
        jr z,.prev_year                 ; был январь
        ld (WorkM),a
        ld hl,(WorkY)
        call Cal_DaysInMonth            ; последний день прошлого месяца
        ld (WorkD),a
        ret
.prev_year:
        ld a,12
        ld (WorkM),a
        ld a,31
        ld (WorkD),a                    ; 31 декабря прошлого года
        ld hl,(WorkY)
        dec hl
        ld (WorkY),hl
        ret

; Перевести WorkY/WorkM/WorkD на день вперёд.
Cal_NextDay:
        ld a,(WorkM)
        ld hl,(WorkY)
        call Cal_DaysInMonth
        ld c,a                          ; C — дней в текущем месяце
        ld a,(WorkD)
        cp c
        jr nc,.next_month               ; был последний день месяца
        inc a
        ld (WorkD),a
        ret
.next_month:
        ld a,1
        ld (WorkD),a
        ld a,(WorkM)
        cp 12
        jr nc,.next_year                ; был декабрь
        inc a
        ld (WorkM),a
        ret
.next_year:
        ld a,1
        ld (WorkM),a                    ; 1 января следующего года
        ld hl,(WorkY)
        inc hl
        ld (WorkY),hl
        ret

; Скопировать текущую дату часов в рабочую.
Cal_WorkFromRtc:
        ld hl,(RtcYear)
        ld (WorkY),hl
        ld a,(RtcMonth)
        ld (WorkM),a
        ld a,(RtcDay)
        ld (WorkD),a
        ret

WorkY:          dw 2000                 ; рабочая дата
WorkM:          db 1
WorkD:          db 1
CalM:           db 0                    ; месяц по счёту Зеллера (3..14)
CalJ:           db 0                    ; век
CalK:           db 0                    ; год внутри века
