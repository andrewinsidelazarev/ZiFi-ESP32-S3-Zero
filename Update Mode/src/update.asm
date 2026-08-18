; Включение сетевого updater нативной прошивки ESP32-S3.
;
; Snapshot не пишет Flash сам. Он перезапускает ESP штатной командой,
; дожидается восстановления сохранённого Wi-Fi, открывает TCP 8267 и показывает
; адрес. Образ передаёт PC-инструмент tools\esp_tool.py; любая клавиша закрывает
; listener, если загрузка ещё не начата.

        DEVICE ZXSPECTRUM48
        ORG #6000

UPDATE_WAIT     equ 12000
UPDATE_ROUNDS   equ 64

Start:
        ld sp,#FF00
        ei
        ld a,2
        call #1601
        ld hl,MsgTitle
        call PrintString
        ld hl,MsgBuild
        call PrintString

        call ZiFi_Init
        jp c,NoZiFi

        ld hl,MsgStarting
        call PrintString
        call WaitReady
        jp c,NoEsp
        call ResetEsp
        jp c,ResetFail
        jp StartNetworkUpdater

; Reset закрывает прежние FTP/HTTP-сокеты. Новая прошивка восстанавливает Wi-Fi
; из LittleFS, поэтому после ACK обязательно ждём READY уже от нового запуска.
ResetEsp:
        ld hl,MsgResettingEsp
        call PrintString
        xor a
        ld (ProtoErr),a
        ld a,CMD_SYS_RESET
        call Proto_SendEmpty
        ret c
        ld a,RESP_ACK
        ld de,UPDATE_WAIT
        call Proto_WaitCmd
        ret c
        jp WaitReady

StartNetworkUpdater:
        xor a
        ld (ProtoErr),a
        ld a,CMD_UPDATE_START
        call Proto_SendEmpty
        jr c,NoEsp
        ld a,RESP_ACK
        ld de,UPDATE_WAIT
        call Proto_WaitCmd
        jr c,NoEsp

        ld a,RESP_UPDATE_START
        call WaitUpdateResult
        jp c,UpdateFail
        ld hl,(ProtoRxLen)
        ld de,7
        or a
        sbc hl,de
        jp nz,UpdateFail
        ld a,(ProtoBuf)
        or a
        jp z,UpdateFail

        ld hl,MsgAddress
        call PrintString
        call PrintAddress
        ld hl,MsgPort
        call PrintString
        ld hl,MsgRun
        call PrintString

WaitAnyKey:
        call AnyKeyPressed
        jr z,WaitAnyKey
.release:
        call AnyKeyPressed
        jr nz,.release

        ld hl,MsgStopping
        call PrintString
        call WaitReady
        jr c,Stopped
        ld a,CMD_UPDATE_STOP
        call Proto_SendEmpty
        jr c,Stopped
        ld a,RESP_ACK
        ld de,UPDATE_WAIT
        call Proto_WaitCmd
        jr c,Stopped
        ld a,RESP_UPDATE_STOP
        ld de,UPDATE_WAIT
        call Proto_WaitCmd

Stopped:
        ld hl,MsgStopped
        call PrintString
Forever:
        jr Forever

NoZiFi:
        ld hl,MsgNoZiFi
        call PrintString
        jr Forever

NoEsp:
        ld hl,MsgNoEsp
        call PrintString
        jr Forever

UpdateFail:
        ld hl,MsgFailed
        call PrintString
        ld a,(ProtoErr)
        or a
        ld hl,MsgNoReply
        jr z,.show
        ld hl,ProtoErrText
.show:
        call PrintString
        ld hl,MsgEndLine
        call PrintString
        jr Forever

ResetFail:
        ld hl,MsgResetFailed
        call PrintString
        ld a,(ProtoErr)
        or a
        ld hl,MsgNoReply
        jr z,.show
        ld hl,ProtoErrText
.show:
        call PrintString
        ld hl,MsgEndLine
        call PrintString
        jr Forever

; Повторять PING, пока нативная прошивка не начнёт отвечать READY.
WaitReady:
        ld b,30
.attempt:
        push bc
        ld a,CMD_PING
        call Proto_SendEmpty
        jr c,.miss
        ld a,RESP_READY
        ld de,UPDATE_WAIT
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

; UPDATE_START может ждать повторного подключения к точке доступа.
WaitUpdateResult:
        ld b,UPDATE_ROUNDS
.round:
        push bc
        ld a,RESP_UPDATE_START
        ld de,0
        call Proto_WaitCmd
        pop bc
        ret nc
        djnz .round
        scf
        ret

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

; Напечатать четыре бинарных октета IP из ответа UPDATE_START.
PrintAddress:
        ld hl,ProtoBuf+1
        ld b,4
.octet:
        ld a,(hl)
        push hl
        push bc
        call PrintByteDec
        pop bc
        pop hl
        inc hl
        djnz .dot
        ret
.dot:
        ld a,'.'
        call PrintChar
        jr .octet

; A=0..255 -> десятичный текст без ведущих нулей.
PrintByteDec:
        ld e,a
        ld d,0
        ld b,'0'
.hundreds:
        ld a,e
        cp 100
        jr c,.hundreds_done
        sub 100
        ld e,a
        inc b
        jr .hundreds
.hundreds_done:
        ld a,b
        cp '0'
        jr z,.tens_start
        call PrintChar
        ld d,1
.tens_start:
        ld b,'0'
.tens:
        ld a,e
        cp 10
        jr c,.tens_done
        sub 10
        ld e,a
        inc b
        jr .tens
.tens_done:
        ld a,b
        cp '0'
        jr nz,.print_tens
        ld a,d
        or a
        jr z,.ones
        ld a,'0'
.print_tens:
        call PrintChar
.ones:
        ld a,e
        add a,'0'
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

MsgTitle:        db 13,"ZiFi ESP32-S3 updater",13,0
MsgBuild:        db "Build: u6-s3-ota",13,0
MsgStarting:     db "Starting network access...",13,0
MsgResettingEsp: db "Resetting ESP...",13,0
MsgAddress:      db "Updater: ",0
MsgPort:         db ":8267",13,0
MsgRun:          db 13,"Run on PC:",13
                 db "py -u tools/esp_tool.py",13
                 db "  --net <IP>",13,13
                 db "ANY KEY = stop updater",13,0
MsgStopping:     db 13,"Stopping updater...",13,0
MsgStopped:      db "Updater stopped.",13,"Reset ZX or run FTP.",13,0
MsgNoZiFi:       db "ZiFi not found!",13,0
MsgNoEsp:        db "ESP does not reply.",13,0
MsgResetFailed:  db "ESP reset failed: ",0
MsgFailed:       db "Update start failed: ",0
MsgNoReply:      db "no reply",0
MsgEndLine:      db 13,0

        INCLUDE "zifi_uart.asm"
        INCLUDE "proto.asm"

        ASSERT $ < #FF00,"update.sna payload overlaps stack"
        SAVESNA "../build/update.sna",Start
