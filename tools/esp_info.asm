; Наглядный просмотр ответа SYS_INFO нативной прошивки ZiFi ESP32-S3 Zero.
;
; За основу взят tests/test_info.asm из эталонного MicroPython-релиза. В отличие
; от старого теста эта программа принимает пакет по объявленной длине, проверяет
; checksum общим парсером проекта и выводит каждое поле отдельной строкой.

        DEVICE ZXSPECTRUM48
        ORG #8000

INFO_WAIT       equ 12000
INFO_RETRIES    equ 30
ATTR_P          equ #5C8D
ATTR_T          equ #5C8F

Start:
        ld sp,#FF00
        ei
        call PrepareScreen
        call DrawTitle

        ld hl,MsgSearching
        call PrintString

        call ZiFi_Init
        jp c,NoZiFi
        ; При повторном запросе очистить не только аппаратную очередь, но и
        ; остаток локальной INIR-порции и состояние пакетного парсера.
        call Proto_ResetRx
        xor a
        ld (ProtoBurstLeft),a

        call WaitReady
        jp c,NoEsp

        call RequestInfo
        jp c,InfoFailed

        call DrawInfo
        jp WaitRefresh

NoZiFi:
        ld hl,MsgNoZiFi
        call PrintError
        call PrintFooter
        jp WaitRefresh

NoEsp:
        ld hl,MsgNoEsp
        call PrintError
        call PrintFooter
        jp WaitRefresh

InfoFailed:
        call PrepareScreen
        call DrawTitle
        ld hl,MsgInfoFailed
        call PrintError
        ld a,(ProtoErr)
        or a
        jr z,.no_detail
        ld hl,MsgEspError
        call PrintString
        ld hl,ProtoErrText
        call PrintString
        call PrintNewLine
.no_detail:
        call PrintFooter
        jp WaitRefresh

; Несколько раз посылать PING, пока ESP не начнёт отвечать READY.
WaitReady:
        ld b,INFO_RETRIES
.attempt:
        push bc
        ld a,CMD_PING
        call Proto_SendEmpty
        jr c,.miss
        ld a,RESP_READY
        ld de,INFO_WAIT
        call Proto_WaitCmd
.miss:
        pop bc
        jr nc,.ready
        djnz .attempt
        scf
        ret
.ready:
        or a
        ret

; Запросить SYS_INFO. ACK и финальный ответ могут находиться в одной аппаратной
; порции, поэтому оба принимает общий очередной парсер, не теряющий хвост.
RequestInfo:
        xor a
        ld (ProtoErr),a
        ld a,CMD_SYS_INFO
        call Proto_SendEmpty
        ret c

        ld a,RESP_ACK
        ld de,INFO_WAIT
        call Proto_WaitCmd
        ret c

        ld a,RESP_SYS_INFO
        ld de,INFO_WAIT
        call Proto_WaitCmd
        ret c

        ld hl,(ProtoRxLen)
        ld a,h
        or l
        jr nz,.has_data
        scf
        ret
.has_data:
        ld de,ProtoBuf
        add hl,de
        ld (InfoEnd),hl
        xor a
        ld (hl),a                       ; безопасный ноль уже зарезервирован
        or a
        ret

DrawInfo:
        call PrepareScreen
        call DrawTitle

        ld a,4
        call SetInk
        ld hl,MsgReady
        call PrintString
        ld a,7
        call SetInk
        call PrintNewLine

        ld hl,KeyFirmware
        ld de,LabelFirmware
        call PrintNamedField
        call PrintNewLine
        ld hl,KeyRam
        ld de,LabelRam
        call PrintNamedField
        ld hl,KeyPsram
        ld de,LabelPsram
        call PrintNamedField
        ld hl,KeyFlash
        ld de,LabelFlash
        call PrintNamedField
        ld hl,KeySketch
        ld de,LabelSketch
        call PrintNamedField
        call PrintNewLine
        ld hl,KeyReset
        ld de,LabelReset
        call PrintNamedField
        ld hl,KeyCore
        ld de,LabelCore
        call PrintNamedField
        ld hl,KeyNetStack
        ld de,LabelNetStack
        call PrintNamedField
        ld hl,KeyControl
        ld de,LabelControl
        call PrintNamedField
        ld hl,KeyVfsBuffer
        ld de,LabelVfsBuffer
        call PrintNamedField
        ld hl,KeyProxy
        ld de,LabelProxy
        call PrintNamedField

        call PrintFooter
        ret

; HL указывает на ключ вида "RAM:", DE — на человекочитаемую подпись.
PrintNamedField:
        ld (FieldLabel),de
        call FindField
        jr c,.missing
        ld (FieldValue),hl
        ld a,1
        jr .remember
.missing:
        xor a
.remember:
        ld (FieldFound),a

        ld a,6
        call SetInk
        ld hl,(FieldLabel)
        call PrintString

        ld a,(FieldFound)
        or a
        jr z,.print_na
        ld a,7
        call SetInk
        ld hl,(FieldValue)
        call PrintFieldValue
        jr .line_done
.print_na:
        ld a,2
        call SetInk
        ld hl,MsgNotAvailable
        call PrintString
.line_done:
        ld a,7
        call SetInk
        jp PrintNewLine

; Найти ключ HL только в начале токена. Это важно: RAM: нельзя принимать за
; суффикс PSRAM:. Выход: CF=0 и HL сразу после ключа; CF=1 — поля нет.
FindField:
        ld (FindKey),hl
        ld de,ProtoBuf
.token:
        ld hl,(InfoEnd)
        ld a,d
        cp h
        jr c,.compare_start
        jr nz,.not_found
        ld a,e
        cp l
        jr nc,.not_found

.compare_start:
        push de
        ld hl,(FindKey)
.compare:
        ld a,(hl)
        or a
        jr z,.found
        ld c,a
        ld a,(de)
        cp c
        jr nz,.mismatch
        inc hl
        inc de
        jr .compare
.found:
        pop bc                          ; отбросить начало совпавшего токена
        ex de,hl                        ; HL = начало значения
        or a
        ret

.mismatch:
        pop de
.skip_token:
        ld hl,(InfoEnd)
        ld a,d
        cp h
        jr c,.read_skip
        jr nz,.not_found
        ld a,e
        cp l
        jr nc,.not_found
.read_skip:
        ld a,(de)
        inc de
        cp ' '
        jr nz,.skip_token
        jr .token
.not_found:
        scf
        ret

; Напечатать значение HL до пробела либо физического конца payload.
PrintFieldValue:
.loop:
        ld de,(InfoEnd)
        ld a,h
        cp d
        jr c,.read
        ret nz
        ld a,l
        cp e
        ret nc
.read:
        ld a,(hl)
        or a
        ret z
        cp ' '
        ret z
        call PrintChar
        inc hl
        jr .loop

PrepareScreen:
        ld a,2
        call #1601                       ; верхнее окно экрана
        ld a,#07                         ; белый текст на чёрном фоне
        ld (ATTR_P),a
        ld (ATTR_T),a
        call #0DAF                       ; полный CLS и сброс позиции печати
        xor a
        out (#FE),a                      ; чёрная рамка
        ret

DrawTitle:
        ld a,1
        call SetBright
        ld a,5
        call SetInk
        ld hl,MsgTitle
        call PrintString
        ld hl,MsgRule
        call PrintString
        ld a,0
        call SetBright
        ld a,7
        jp SetInk

PrintError:
        ld a,2
        call SetInk
        call PrintString
        ld a,7
        call SetInk
        jp PrintNewLine

PrintFooter:
        call PrintNewLine
        ld a,6
        call SetInk
        ld hl,MsgRefresh
        call PrintString
        ld a,7
        jp SetInk

; Остановиться на экране. После нажатия и отпускания любой клавиши повторить
; запрос с чистого экрана и заново инициализированным стеком.
WaitRefresh:
        call AnyKeyPressed
        jr z,WaitRefresh
.release:
        call AnyKeyPressed
        jr nz,.release
        jp Start

; Z=0, если нажата хотя бы одна клавиша в любой из восьми полустрок.
AnyKeyPressed:
        ld bc,#FEFE
        ld d,8
.row:
        in a,(c)
        and #1F
        cp #1F
        ret nz
        rlc b
        dec d
        jr nz,.row
        xor a
        ret

SetInk:
        push af
        ld a,16
        call PrintChar
        pop af
        jp PrintChar

SetBright:
        push af
        ld a,19
        call PrintChar
        pop af
        jp PrintChar

PrintNewLine:
        ld a,13
        jp PrintChar

PrintString:
        ld a,(hl)
        or a
        ret z
        call PrintChar
        inc hl
        jr PrintString

PrintChar:
        push bc
        push de
        push hl
        rst #10
        pop hl
        pop de
        pop bc
        ret

MsgTitle:        db "ZiFi ESP32-S3 information",13,0
MsgRule:         db "--------------------------",13,0
MsgSearching:    db "Looking for ZiFi and ESP...",13,0
MsgReady:        db "ZiFi/ESP link: READY",13,0
MsgNoZiFi:       db "ZiFi interface not found",0
MsgNoEsp:        db "ESP firmware does not reply",0
MsgInfoFailed:   db "SYS_INFO request failed",0
MsgEspError:     db "ESP error: ",0
MsgNotAvailable: db "n/a",0
MsgRefresh:      db "ANY KEY - refresh",0

KeyFirmware:     db "FW:",0
KeyRam:          db "RAM:",0
KeyPsram:        db "PSRAM:",0
KeyFlash:        db "Flash:",0
KeySketch:       db "Sketch:",0
KeyReset:        db "RST:",0
KeyCore:         db "CORE:",0
KeyNetStack:     db "NETSTK:",0
KeyControl:      db "CTRL:",0
KeyVfsBuffer:    db "VFSBUF:",0
KeyProxy:        db "PROXY:",0

LabelFirmware:   db "Firmware: ",0
LabelRam:        db "Free RAM: ",0
LabelPsram:      db "PSRAM: ",0
LabelFlash:      db "Flash: ",0
LabelSketch:     db "Application: ",0
LabelReset:      db "Reset reason: ",0
LabelCore:       db "Cores: ",0
LabelNetStack:   db "Network stack: ",0
LabelControl:    db "Control buffer: ",0
LabelVfsBuffer:  db "VFS buffers: ",0
LabelProxy:      db "HTTP proxy: ",0

InfoEnd:         dw ProtoBuf
FindKey:         dw 0
FieldLabel:      dw 0
FieldValue:      dw 0
FieldFound:      db 0

        INCLUDE "zifi_uart.asm"
        INCLUDE "proto.asm"

        ASSERT $ < #FF00,"esp_info.sna payload overlaps stack"
        SAVESNA "esp_info.sna",Start
