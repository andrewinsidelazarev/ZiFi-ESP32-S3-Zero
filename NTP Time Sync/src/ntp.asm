; Разбор ответа ESP и решение, надо ли трогать часы.
;
; ESP отдаёт время строкой из 14 цифр ГГГГММДДЧЧММСС, уже с поправкой на
; часовой пояс из поля time: файла zifi.ini — пересчитывать ничего не нужно.
; Строка из одних нулей означает, что время получить не удалось.

NTP_ANSWER_LEN  equ 14

; Перевести пару ASCII-цифр по (HL) в один байт BCD.
; Выход: A — значение, HL сдвинут на два символа, CF=1 если символы не цифры.
; Приём взят из write_rtc самого ZiFi (процедура code_time_rtc).
Ntp_PairToBcd:
        ld a,(hl)
        call Ntp_CheckDigit
        ret c
        and #0F
        rlca
        rlca
        rlca
        rlca
        ld e,a
        inc hl
        ld a,(hl)
        call Ntp_CheckDigit
        ret c
        and #0F
        or e
        inc hl
        or a                            ; CF=0: разбор удался
        ret

; CF=1, если A не является ASCII-цифрой.
Ntp_CheckDigit:
        cp '0'
        ret c
        cp '9'+1
        ccf
        ret

; Разобрать ответ в ProtoBuf в NtpTime.
; Выход: CF=1 — ответ негодный (не та длина, не цифры либо все нули).
Ntp_Parse:
        ld hl,(ProtoRxLen)
        ld de,NTP_ANSWER_LEN
        or a
        sbc hl,de
        jr nz,.bad

        ; Год приходит четырьмя цифрами, часам нужны две младшие.
        ld hl,ProtoBuf+2
        ld de,NtpTime
        ld b,RTC_FIELDS
.loop:
        push bc
        push de
        call Ntp_PairToBcd
        pop de
        pop bc
        ret c
        ld (de),a
        inc de
        djnz .loop

        ; Все нули — признак неудачи на стороне ESP.
        ld hl,NtpTime
        ld b,RTC_FIELDS
.zero:
        ld a,(hl)
        or a
        jr nz,.good
        inc hl
        djnz .zero
.bad:
        scf
        ret
.good:
        or a
        ret

; Сравнить часы с полученным временем по полям до минут включительно.
; Секунды намеренно не сравниваем: расхождение внутри одной минуты часы не
; правит. Если отличается любое поле от года до минут, разница составляет не
; менее минуты — тогда время записывается.
; Выход: Z=1 — правка не нужна.
Ntp_NeedUpdate:
        ld hl,NtpTime
        ld de,RtcTime
        ld b,RTC_FIELDS-1               ; без секунд
.loop:
        ld a,(de)
        cp (hl)
        ret nz
        inc hl
        inc de
        djnz .loop
        xor a                           ; Z=1: часы уже верны
        ret

NtpTime:
        ds RTC_FIELDS                   ; год, месяц, число, часы, минуты, секунды
