; Пользовательский WMF-обновлятор. Плагин читает /zifi/zifi.ini, соединяет ESP
; с Wi-Fi и запускает автономную загрузку. Сам firmware.bin через Z80 не идёт.

PLUGIN:
        push ix
        ld a,(ix+29)
        ld (ConfigPanelDevice),a
        ld a,1
        call WC_INT_PL
        call WC_GEDPL
        call Ui_Open
        call Ui_Draw

        call Config_Load
        jp c,.config_error

        ld hl,UiDetecting
        call Ui_SetStatus
        call Ui_Draw
        call Updater_StartLink
        jp c,.link_error

        ld hl,UiChecking
        call Ui_SetStatus
        xor a
        call Ui_SetProgress
        call Ui_Draw
        call Updater_CheckPublished
        jp c,.update_error

.confirm:
        ei
        halt
        call WC_ESC
        jp nz,.cancel
        ld bc,#BFFE                    ; ENTER: младший бит строки H/J/K/L/Enter
        in a,(c)
        and 1
        jr nz,.confirm
.release_enter:
        ei
        halt
        in a,(c)
        and 1
        jr z,.release_enter

        ld hl,UiManifest
        call Ui_SetStatus
        xor a
        call Ui_SetProgress
        call Ui_Draw
        xor a
        ld (ProtoErr),a
        ld a,CMD_ONLINE_UPDATE
        call Proto_SendEmpty
        jr c,.update_error
        ld a,RESP_ACK
        ld de,UPDATER_WAIT
        call Proto_WaitCmd
        jr c,.update_error
        ld a,RESP_ONLINE_UPDATE
        call Updater_WaitResult
        jr c,.update_error
        ld hl,(ProtoRxLen)
        ld de,33
        or a
        sbc hl,de
        jr nz,.update_error
        ld a,(ProtoBuf)
        or a
        jr z,.update_error

        ld hl,UiRestarting
        call Ui_SetStatus
        ld a,100
        call Ui_SetProgress
        call Ui_Draw
        call Updater_WaitRestart
        jr c,.restart_unknown
        call Updater_ReadVersion
        ld hl,UiCompleted
        call Ui_SetStatus
        call Ui_Draw
        jr .wait_exit

.restart_unknown:
        ld hl,UiRestartUnknown
        call Ui_SetStatus
        call Ui_Draw
        jr .wait_exit

.config_error:
        ld a,(ConfigError)
        cp 1
        ld hl,UiErrorSd
        jr z,.fatal
        cp 2
        ld hl,UiErrorDir
        jr z,.fatal
        ld hl,UiErrorIni
        jr .fatal

.link_error:
        ld a,(UpdaterError)
        cp 1
        ld hl,UiErrorNoZifi
        jr z,.fatal
        cp 2
        ld hl,UiErrorFirmware
        jr z,.fatal
        ld hl,UiErrorWifi
        jr .fatal

.update_error:
        ld a,(ProtoErr)
        or a
        ld hl,UiErrorUpdate
        jr z,.fatal
        ld hl,ProtoErrText
.fatal:
        call Ui_SetStatus
        call Ui_Draw
.wait_exit:
        ei
        halt
        call WC_ESC
        jr z,.wait_exit
        jr .exit

.cancel:
        ld hl,UiCancelled
        call Ui_SetStatus
        call Ui_Draw
.exit:
        call Config_RestoreRoot
        ld ix,UpdateWindow
        call WC_RRESB
        xor a
        pop ix
        ret

UPDATER_WAIT        equ 12000
UPDATER_IDLE_TICKS  equ 250             ; примерно 5 секунд до проверки PING
UPDATER_SILENCE     equ 60              ; около 5 минут без единого ответа

; Проверить двоичный протокол, показать текущую версию и передать zifi.ini.
Updater_StartLink:
        xor a
        ld (UpdaterError),a
        call ZiFi_Init
        jr nc,.zifi_ok
        ld a,1
        ld (UpdaterError),a
        scf
        ret
.zifi_ok:
        call Updater_WaitReady
        jr nc,.ready
        ld a,2
        ld (UpdaterError),a
        scf
        ret
.ready:
        call Updater_ReadVersion

        ld hl,UiConnecting
        call Ui_SetStatus
        call Ui_Draw
        xor a
        ld (ProtoErr),a
        ld a,CMD_WIFI_INI
        ld hl,IniBuffer
        ld bc,(IniLength)
        call Proto_Send
        jr c,.wifi_fail
        ld a,RESP_ACK
        ld de,UPDATER_WAIT
        call Proto_WaitCmd
        jr c,.wifi_fail
        ld a,RESP_WIFI_INI
        call Updater_WaitResult
        jr c,.wifi_fail
        ld a,(ProtoBuf)
        or a
        jr z,.wifi_fail
        or a
        ret
.wifi_fail:
        ld a,3
        ld (UpdaterError),a
        scf
        ret

Updater_WaitReady:
        ld b,30
.try:
        push bc
        ld a,CMD_PING
        call Proto_SendEmpty
        ld a,RESP_READY
        ld de,UPDATER_WAIT
        call Proto_WaitCmd
        pop bc
        ret nc
        djnz .try
        scf
        ret

; Прочитать только опубликованный manifest. Flash не открывается на запись.
; Ответ: [1][relation][available version ASCII]. Enter разрешён при любом
; relation, включая ONLINE_UPDATE_SAME — это восстановительная переустановка.
Updater_CheckPublished:
        xor a
        ld (ProtoErr),a
        ld a,CMD_ONLINE_UPDATE_CHECK
        call Proto_SendEmpty
        ret c
        ld a,RESP_ACK
        ld de,UPDATER_WAIT
        call Proto_WaitCmd
        ret c
        ld a,RESP_ONLINE_UPDATE_CHECK
        call Updater_WaitResult
        ret c
        ld hl,(ProtoRxLen)
        ld de,3
        or a
        sbc hl,de
        jr c,.bad
        ld a,(ProtoBuf)
        or a
        jr z,.bad
        ld hl,(ProtoRxLen)
        ld de,ProtoBuf
        add hl,de
        ld (hl),0
        ld hl,ProtoBuf+2
        call Ui_SetAvailable

        ld a,(ProtoBuf+1)
        cp ONLINE_UPDATE_SAME
        ld hl,UiSameVersion
        jr z,.show
        cp ONLINE_UPDATE_NEWER
        ld hl,UiNewerVersion
        jr z,.show
        cp ONLINE_UPDATE_OLDER
        ld hl,UiOlderVersion
        jr z,.show
        ld hl,UiDifferentVersion
.show:
        call Ui_SetStatus
        call Ui_Draw
        or a
        ret
.bad:
        scf
        ret

; SYS_INFO не является причиной запрещать восстановительную переустановку:
; если версия не читается, оставляем понятную заглушку и продолжаем.
Updater_ReadVersion:
        ld a,CMD_SYS_INFO
        call Proto_SendEmpty
        ld a,RESP_ACK
        ld de,UPDATER_WAIT
        call Proto_WaitCmd
        ret c
        ld a,RESP_SYS_INFO
        ld de,UPDATER_WAIT
        call Proto_WaitCmd
        ret c
        ld hl,ProtoBuf
        ld bc,(ProtoRxLen)
        add hl,bc
        ld (hl),0
        call Updater_FindVersion
        call Ui_SetVersion
        call Ui_Draw
        ret

Updater_FindVersion:
        ld hl,ProtoBuf
.scan:
        ld a,(hl)
        or a
        jr z,.absent
        cp 'F'
        jr nz,.next
        inc hl
        ld a,(hl)
        cp 'W'
        jr nz,.scan
        inc hl
        ld a,(hl)
        cp ':'
        jr nz,.scan
        inc hl
        ret
.next:
        inc hl
        jr .scan
.absent:
        ld hl,UiUnknown
        ret

; Ожидание долгой сетевой операции. При CHECK/ONLINE_UPDATE одновременно
; обрабатывает индикатор [этап][проценты]. Ошибка #EE возвращается сразу: так
; старое firmware честно сообщает, что команда ещё не поддерживается.
Updater_WaitResult:
        ld (UpdaterWanted),a
        ld a,UPDATER_SILENCE
        ld (UpdaterSilence),a
        ld hl,UPDATER_IDLE_TICKS
        ld (UpdaterTicks),hl
.loop:
        call Proto_Poll
        jr nc,.idle
        ld hl,UpdaterWanted
        cp (hl)
        jr z,.got
        cp RESP_ERROR
        jr z,.error
        cp RESP_READY
        jr z,.alive
        cp EVT_ONLINE_UPDATE_PROGRESS
        jr nz,.loop
        call Updater_ShowProgress
.alive:
        ld a,UPDATER_SILENCE
        ld (UpdaterSilence),a
        ld hl,UPDATER_IDLE_TICKS
        ld (UpdaterTicks),hl
        jr .loop
.error:
        ld a,1
        ld (ProtoErr),a
        call Proto_SaveErr
        scf
        ret
.idle:
        ei
        halt
        ld hl,(UpdaterTicks)
        dec hl
        ld (UpdaterTicks),hl
        ld a,h
        or l
        jr nz,.loop
        ld hl,UPDATER_IDLE_TICKS
        ld (UpdaterTicks),hl
        ld hl,UpdaterSilence
        dec (hl)
        jr z,.silent
        ld a,CMD_PING
        call Proto_SendEmpty
        jr .loop
.silent:
        scf
        ret
.got:
        or a
        ret

Updater_ShowProgress:
        ld hl,(ProtoRxLen)
        ld de,2
        or a
        sbc hl,de
        ret nz
        ld a,(ProtoBuf+1)
        cp 101
        ret nc
        call Ui_SetProgress
        ld a,(ProtoBuf)
        cp 1
        ld hl,UiManifest
        jr z,.show
        cp 2
        ld hl,UiDownloading
        jr z,.show
        cp 3
        ld hl,UiVerifying
        ret nz
.show:
        call Ui_SetStatus
        jp Ui_Draw

; Финальный ответ уже физически отправлен перед ESP.restart(). Ждём паузу,
; чтобы не принять старый READY, затем ищем READY от новой загрузки.
Updater_WaitRestart:
        ld b,60                         ; около секунды на начало reset
.pause:
        ei
        halt
        djnz .pause
        jp Updater_WaitReady

        INCLUDE "ui.asm"

UpdaterError:       db 0
UpdaterWanted:      db 0
UpdaterSilence:     db 0
UpdaterTicks:       dw 0
