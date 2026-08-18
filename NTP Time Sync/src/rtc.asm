; Часы реального времени Mr.Gluk (Z-контроллер ZX Evolution).
;
; #EFF7 — не «порт часов», а вентиль всего Z-контроллера: на том же чипе висит
; и RTC, и FIFO PS/2-клавиатуры. Доступ открывается записью #80, закрывается
; #00. Пока вентиль закрыт, IN #BFF7 отдаёт #FF.
;
; ВАЖНО: закрывать вентиль внутри Wild Commander НЕЛЬЗЯ. WC читает
; PC-клавиатуру через тот же Mr.Gluk, и после #EFF7 <- #00 он перестаёт
; получать нажатия — с виду зависает. Плагин оставляет вентиль открытым, как
; это делает SETime; закрывает его только самостоятельная программа вроде
; самого ZiFi, которая распоряжается портом целиком.
;
; Значения регистров — BCD. Регистр #0B на время доступа переводится в режим
; SET (#82), после записи возвращается в рабочий (#02).

RTC_GATE        equ #EFF7               ; вентиль Z-контроллера: #80 открыть, #00 закрыть
RTC_ADDR        equ #DFF7               ; выбор регистра
RTC_DATA        equ #BFF7               ; данные выбранного регистра

RTC_REG_SECONDS equ #00
RTC_REG_MINUTES equ #02
RTC_REG_HOURS   equ #04
RTC_REG_DATE    equ #07
RTC_REG_MONTH   equ #08
RTC_REG_YEAR    equ #09
RTC_REG_B       equ #0B                 ; режим: #82 — правка, #02 — рабочий

; Открыть вентиль и перевести часы в режим правки.
Rtc_Begin:
        ld bc,RTC_GATE
        ld a,#80
        out (c),a
        ld a,RTC_REG_B
        ld b,high RTC_ADDR
        out (c),a
        ld a,#82
        ld b,high RTC_DATA
        out (c),a
        ret

; Вернуть часам рабочий режим. Вентиль НЕ закрываем — см. пояснение ниже.
Rtc_End:
        ld bc,RTC_ADDR
        ld a,RTC_REG_B
        out (c),a
        ld a,#02
        ld b,high RTC_DATA
        out (c),a
        ; Записи #EFF7 <- #00 здесь намеренно нет, хотя write_rtc в самом ZiFi
        ; вентиль закрывает. ZiFi — отдельная программа и распоряжается портом
        ; целиком, а мы работаем внутри Wild Commander, который читает
        ; PC-клавиатуру через тот же Mr.Gluk. Закрытый вентиль лишает его ввода
        ; (IN #BFF7 отдаёт #FF), и WC выглядит зависшим: время записано, а
        ; клавиши не доходят. Проверено на железе. Ровно по этой причине в
        ; исходниках SETime такая же строка оставлена закомментированной.
        ret

; Прочитать регистр A. Выход: A — значение BCD.
Rtc_ReadReg:
        ld bc,RTC_ADDR
        out (c),a
        ld b,high RTC_DATA
        in a,(c)
        ret

; Записать в регистр C значение A.
Rtc_WriteReg:
        push af
        ld a,c
        ld bc,RTC_ADDR
        out (c),a
        pop af
        ld b,high RTC_DATA
        out (c),a
        ret

; Снять текущее время часов в RtcTime (BCD, порядок как в NtpTime).
Rtc_Read:
        call Rtc_Begin
        ld hl,RtcTime
        ld de,RtcRegs
        ld b,RTC_FIELDS
.loop:
        push bc
        ld a,(de)
        call Rtc_ReadReg
        ld (hl),a
        inc hl
        inc de
        pop bc
        djnz .loop
        jp Rtc_End

; Записать в часы время из NtpTime.
Rtc_Write:
        call Rtc_Begin
        ld hl,NtpTime
        ld de,RtcRegs
        ld b,RTC_FIELDS
.loop:
        push bc
        ld a,(de)
        ld c,a
        ld a,(hl)
        call Rtc_WriteReg
        inc hl
        inc de
        pop bc
        djnz .loop
        jp Rtc_End

; Порядок полей одинаков для чтения, записи и сравнения.
RtcRegs:
        db RTC_REG_YEAR
        db RTC_REG_MONTH
        db RTC_REG_DATE
        db RTC_REG_HOURS
        db RTC_REG_MINUTES
        db RTC_REG_SECONDS
RTC_FIELDS equ $ - RtcRegs

RtcTime:
        ds RTC_FIELDS                   ; год, месяц, число, часы, минуты, секунды
