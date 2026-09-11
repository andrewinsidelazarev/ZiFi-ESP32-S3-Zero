; Часы реального времени Mr.Gluk (Z-контроллер ZX Evolution), только чтение.
;
; Регистры совместимы с MC146818: 0 секунды, 2 минуты, 4 часы, 7 число,
; 8 месяц, 9 год (две цифры), #0A — бит 7 «идёт обновление», #0B — режим:
; бит 2 = 1 значит двоичные значения, иначе BCD. Значения обычно BCD:
; каждая десятичная цифра — отдельные 4 бита, «37» хранится как #37.
;
; Порты: #EFF7 — вентиль Z-контроллера (#80 открыть; закрывать внутри WC
; нельзя — он читает PS/2-клавиатуру через тот же чип), #DFF7 — выбор
; регистра, #BFF7 — данные. Обработчик прерывания WC сам выбирает регистр
; клавиатуры (#F0) через #DFF7, поэтому пара «выбрать регистр — прочитать»
; выполняется с запрещёнными прерываниями: иначе между OUT и IN прерывание
; подменило бы регистр, и вместо минут пришёл бы скан-код.
RTC_GATE        equ #EFF7
RTC_ADDR        equ #DFF7
RTC_DATA        equ #BFF7
RTC_REG_SEC     equ #00
RTC_REG_MIN     equ #02
RTC_REG_HOUR    equ #04
RTC_REG_DAY     equ #07
RTC_REG_MONTH   equ #08
RTC_REG_YEAR    equ #09
RTC_REG_A       equ #0A
RTC_REG_B       equ #0B

; Открыть вентиль Z-контроллера. Внутри WC он и так открыт (через него идёт
; клавиатура), запись нужна один раз при запуске, как в NTPTIME.WMF.
Rtc_Open:
        ld bc,RTC_GATE
        ld a,#80
        out (c),a
        ret

; Прочитать дату и время в RtcSec..RtcYear (двоичные значения, год полный).
; Вызывается каждый кадр.
Rtc_Read:
        ; Раз в секунду часы обновляют регистры; в это время (бит 7 регистра A)
        ; значения могут быть наполовину старыми. Ждём конца обновления, но
        ; не вечно: 256 попыток (B=0 для DJNZ означает 256 повторов).
        ld b,0
.wait_update:
        push bc
        ld a,RTC_REG_A
        call Rtc_ReadReg
        pop bc
        rlca                            ; бит 7 в CF
        jr nc,.stable
        djnz .wait_update
.stable:
        ld a,RTC_REG_B
        call Rtc_ReadReg
        and %00000100                   ; DM: 1 — двоичный режим
        ld (RtcBinary),a
        ld a,RTC_REG_SEC
        call Rtc_ReadValue
        ld (RtcSec),a
        ld a,RTC_REG_MIN
        call Rtc_ReadValue
        ld (RtcMin),a
        ld a,RTC_REG_HOUR
        call Rtc_ReadValue
        ld (RtcHour),a
        ld a,RTC_REG_DAY
        call Rtc_ReadValue
        ld (RtcDay),a
        ld a,RTC_REG_MONTH
        call Rtc_ReadValue
        ld (RtcMonth),a
        ld a,RTC_REG_YEAR
        call Rtc_ReadValue
        ld l,a
        ld h,0
        ld de,2000                      ; часы хранят только две цифры года
        add hl,de
        ld (RtcYear),hl
        ret

; Прочитать регистр A как число: BCD переводится в двоичное.
Rtc_ReadValue:
        call Rtc_ReadReg
        ld c,a
        ld a,(RtcBinary)
        or a
        ld a,c
        ret nz                          ; уже двоичное
        ; BCD -> двоичное: старшая цифра * 10 + младшая
        ld c,a
        and #F0                         ; старшая цифра в битах 7..4
        rrca
        rrca
        rrca
        rrca                            ; ...теперь в битах 3..0
        ld b,a
        add a,a                         ; *2
        add a,a                         ; *4
        add a,b                         ; *5
        add a,a                         ; старшая * 10
        ld b,a
        ld a,c
        and #0F                         ; младшая цифра
        add a,b
        ret

; Прочитать регистр A. Выход: A — значение. Сохраняет BC, DE, HL.
Rtc_ReadReg:
        push bc
        ld bc,RTC_ADDR
        di                              ; прерывание не должно вклиниться
        out (c),a                       ; выбрать регистр
        ld b,high RTC_DATA
        in a,(c)                        ; прочитать его значение
        ei
        pop bc
        ret

RtcSec:         db 0                    ; секунды 0..59
RtcMin:         db 0                    ; минуты 0..59
RtcHour:        db 0                    ; часы 0..23
RtcDay:         db 1                    ; число 1..31
RtcMonth:       db 1                    ; месяц 1..12
RtcYear:        dw 2000                 ; год полностью
RtcBinary:      db 0                    ; не ноль — часы в двоичном режиме
