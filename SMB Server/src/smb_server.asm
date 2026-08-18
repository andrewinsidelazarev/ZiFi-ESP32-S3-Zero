; Точка входа плагина. PLUGIN обязан быть первым байтом по адресу #8000.
;
; Что делает плагин: находит zifi.ini, отдаёт его на ESP целиком, просит поднять
; SMB2/SMB3-сервер и дальше отвечает только на файловые запросы. Сеть, TCP,
; проверка пароля, подпись SMB и разбор пакетов целиком находятся на ESP32-S3.
; Z80 видит простой двоичный VFS и работает с SD через API Wild Commander.
;
; IX при входе указывает на служебную структуру Wild Commander; поле IX+29 —
; устройство активной панели, его надо забрать до того, как IX займут окна.

PLUGIN:
        push ix
        ld a,(ix+29)
        ld (ConfigPanelDevice),a
        ld a,1
        call WC_INT_PL                   ; WC не должен перерисовывать часы под окном
        call WC_GEDPL
        call Ui_Open

        ld hl,UiStageConfig
        call Ui_SetStatus
        call Ui_Draw
        call Config_Load                 ; найти и прочитать zifi.ini
        jr c,.config_error

        ld hl,UiStageZifi
        call Ui_SetStatus
        call Ui_Draw
        call Link_Start                  ; ZiFi + готовность ESP + Wi-Fi + SMB
        jr c,.network_error

        ld hl,UiStageListening
        call Ui_SetStatus
        call Ui_BuildIp
        call Ui_Draw

.server_loop:
        ; Vfs_Serve разбирает всё, что уже пришло, и отвечает на файловые
        ; запросы. HALT отдаёт время WC и не создаёт busy-loop на Z80.
        ei
        call Vfs_Serve
        call WC_ESC
        jr nz,.exit
        halt
        jr .server_loop

.config_error:
        ld a,(ConfigError)
        cp 1
        ld hl,UiErrorSd
        jr z,.fatal
        cp 2
        ld hl,UiErrorDir
        jr z,.fatal
        cp 3
        ld hl,UiErrorIni
        jr z,.fatal
        ld hl,UiErrorConfig
        jr .fatal

.network_error:
        ; LinkError: 1 — нет ZiFi, 2 — ESP молчит, 3 — Wi-Fi/IP, 4 — SMB не поднялся.
        ld a,(LinkError)
        cp 1
        ld hl,UiErrorNoZifi
        jr z,.fatal
        cp 2
        ld hl,UiErrorFirmware
        jr z,.fatal
        cp 3
        jr nz,.err_server
        call Link_BuildFail            ; HL -> "ini=NN <причина от ESP>"
        jr .fatal
.err_server:
        ; Причину отказа SMB присылает ESP пакетом #EE — показываем её.
        ld hl,UiErrorServer
        ld a,(ProtoErr)
        or a
        jr z,.fatal
        ld hl,ProtoErrText

.fatal:
        call Ui_SetStatus
        call Ui_Draw
.fatal_wait:
        ei
        halt
        call WC_ESC
        jr z,.fatal_wait

.exit:
        call Link_Stop                   ; попросить ESP закрыть SMB-сервер
        ld ix,SmbWindow
        call WC_RRESB
        ; При возврате Wild Commander сам восстанавливает настройки прерываний.
        xor a                            ; код возврата: обычный выход из плагина
        pop ix
        ret

; --- запуск и остановка обмена ----------------------------------------------

; Единица опроса очереди ZiFi. Ею НЕ измеряется длительность операции: время
; подключения к сети заранее неизвестно, а угаданные пределы уже приводили к
; ложным ошибкам. Долгий ответ ждём столько, сколько нужно, а живость ESP
; проверяем пингом: пришёл READY — значит модуль работает, ждём дальше.
LINK_TRY        equ 12000
LINK_PROBE      equ 10                   ; проб между пингами живости
LINK_SILENCE    equ 15                   ; столько пингов без READY = ESP мёртв

; Поднять весь тракт. Выход: CF=1 и LinkError с причиной.
Link_Start:
        xor a
        ld (LinkError),a
        call ZiFi_Init                   ; режим API 1 и очистка очереди приёма
        jr nc,.zifi_ok
        ld a,1
        ld (LinkError),a
        scf
        ret
.zifi_ok:
        ; ESP перезапускается ПЕРЕД работой. RESET ZX её не трогает, поэтому без
        ; этого плагин каждый раз садился на кучу, оставшуюся от прошлого сеанса:
        ; свободной памяти много, но она раздроблена, и постоянные буферы уже не
        ; выделяются целым куском. На железе это выглядело как чтение нулевой
        ; длины у файлов больше сектора (err=retr:uart-rx-memory) и как отказ
        ; кольца приёма (err=stor-buffer:MemoryError). После сброса main.py
        ; стартует заново и резервирует буферы первым делом.
        ; Ответ не проверяем: ESP может быть уже в сбросе или ещё не готова —
        ; готовность всё равно устанавливается циклом PING ниже.
        ld a,CMD_SYS_RESET
        call Proto_SendEmpty
        ; Готовность ESP: PING повторяем, пока не ответит READY. ESP сам в линию
        ; ничего не пишет, поэтому мусора тут не бывает и ловить нечего.
        ; Попыток столько же, сколько в update.sna после его SYS_RESET: там эта
        ; величина оставлена с запасом для запуска нативной прошивки и Wi-Fi.
        ld b,30
.ping:
        push bc
        ld a,CMD_PING
        call Proto_SendEmpty
        ld a,RESP_READY
        ld de,LINK_TRY
        call Proto_WaitCmd
        pop bc
        jr nc,.ready
        djnz .ping
        ld a,2
        ld (LinkError),a
        scf
        ret
.ready:
        ; Показать версию прошивки ESP: раньше её давал AT+GMR, теперь SYS_INFO.
        ; Это же служит доказательством, что двоичный обмен реально работает.
        ld a,CMD_SYS_INFO
        call Proto_SendEmpty
        ld a,RESP_ACK
        ld de,LINK_TRY
        call Proto_WaitCmd
        ld a,RESP_SYS_INFO
        call Link_WaitResult
        jr c,.no_info
        ; Ответ не завершён нулём — дописываем его перед разбором.
        ld hl,ProtoBuf
        ld bc,(ProtoRxLen)
        add hl,bc
        ld (hl),0
        ; В окне показываем только версию (после "MP:"), а не весь ответ с RAM/Flash.
        call Link_FindVersion
        call Ui_SetFirmware
        jr .info_done
.no_info:
        ld hl,UiFirmwareSilent
        call Ui_SetFirmware
.info_done:
        ld hl,UiStageWifiJoin
        call Ui_SetStatus
        call Ui_Draw

        ; Отдаём zifi.ini целиком: разбор SSID/пароля/смещения GMT — на ESP.
        ld a,CMD_WIFI_INI
        ld hl,IniBuffer
        ld bc,(IniLength)
        call Proto_Send
        ld a,RESP_ACK
        ld de,LINK_TRY
        call Proto_WaitCmd
        ; Подключение к сети занимает секунды, поэтому ждём с повтором.
        ld a,RESP_WIFI_INI
        call Link_WaitResult
        jr c,.wifi_fail
        ld a,(ProtoBuf)
        or a
        jr z,.wifi_fail
        ; Запомнить выданный IP для окна статуса.
        ld hl,ProtoBuf+1
        ld de,LocalIp
        ld bc,4
        ldir

        ; IP выводим СРАЗУ: если дальше сорвётся SMB, пользователь всё равно
        ; увидит, что сеть поднялась и адрес получен.
        call Ui_BuildIp
        ld hl,UiStageSmb
        call Ui_SetStatus
        call Ui_Draw

        ; Просим ESP поднять SMB-сервер. В одном пакете задаём порт, имя ресурса,
        ; NetBIOS-имя, рабочую группу, логин и пароль. Значит эти параметры можно
        ; изменить в WMF без перепрошивки ESP32-S3.
        ld a,CMD_SMB_START
        ld hl,LinkSmbStart
        ld bc,LinkSmbStartLen
        call Proto_Send
        ld a,RESP_ACK
        ld de,LINK_TRY
        call Proto_WaitCmd
        ld a,RESP_SMB_START
        call Link_WaitResult
        jr c,.smb_fail
        ld a,(ProtoBuf)
        or a
        jr z,.smb_fail
        ; Ответ содержит [успех][порт LE16][признак NBNS]. Проверка длины
        ; защищает окно от старой прошивки с ответом другого формата.
        ld hl,(ProtoRxLen)
        ld a,h
        or a
        jr nz,.smb_answer_ready
        ld a,l
        cp 4
        jr c,.smb_fail
.smb_answer_ready:
        ld hl,(ProtoBuf+1)
        ld (LinkSmbPort),hl
        ld a,(ProtoBuf+3)
        ld (LinkNetbios),a
        call Ui_BuildPort
        call Ui_BuildNetbios
        or a
        ret

.wifi_fail:
        ld a,3
        ld (LinkError),a
        scf
        ret
.smb_fail:
        ld a,4
        ld (LinkError),a
        scf
        ret

; Собрать стойкую строку отказа Wi-Fi: сколько байт zifi.ini прочитано с SD и
; что ответил ESP. Висит в окне до Esc, поэтому её всегда можно прочитать.
; Выход: HL -> строка с нулём.
Link_BuildFail:
        ld hl,LinkIniMsg               ; префикс "ini="
        ld de,LinkFailBuf
        call CopyZNoTerm
        ld hl,(IniLength)              ; число прочитанных байт
        call U16_ToDec
        ld a,' '
        ld (de),a
        inc de
        ; Причина: текст от ESP (0xEE) либо пометка об отсутствии ответа.
        ld a,(ProtoErr)
        or a
        ld hl,ProtoErrText
        jr nz,.copy
        ld hl,LinkNoReply
.copy:
        call CopyZNoTerm
        xor a
        ld (de),a
        ld hl,LinkFailBuf
        ret

; Найти в ответе SYS_INFO маркер "FW:" и вернуть HL на текст версии за ним.
; Старая Native-сборка присылала "Native C++ native-..."; её служебный префикс
; пропускаем, чтобы и без обновления ESP в поле остался один build identifier.
; Если маркера нет — HL на строку-заглушку.
; Разрушает A/DE.
Link_FindVersion:
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
        jr nz,.scan            ; после F не W — продолжаем от текущего
        inc hl
        ld a,(hl)
        cp ':'
        jr nz,.scan
        inc hl                 ; HL на первом символе версии
        push hl
        ld de,LinkNativePrefix
.native_prefix:
        ld a,(de)
        or a
        jr z,.native_match
        cp (hl)
        jr nz,.native_miss
        inc de
        inc hl
        jr .native_prefix
.native_match:
        pop de                 ; отбросить сохранённый адрес до префикса
        ret
.native_miss:
        pop hl                 ; другой формат FW: вернуть без изменений
        ret
.next:
        inc hl
        jr .scan
.absent:
        ld hl,UiFirmwareSilent
        ret

; Ждать ответ A столько, сколько потребуется. Никаких пределов по времени:
; выход только по самому ответу либо по молчанию ESP на пинги.
; Пока ESP занят (например, подключается к сети), наши пинги копятся в его
; приёмном буфере и получают READY сразу после освобождения — счётчик молчания
; сбрасывается, и ожидание продолжается.
; Выход: CF=0 — ответ получен, CF=1 — ESP не отвечает даже на пинг.
Link_WaitResult:
        ld (LinkWant),a
        ld a,LINK_SILENCE
        ld (LinkSilence),a
.outer:
        ld b,LINK_PROBE
.wait:
        push bc
        ld a,(LinkWant)
        ld de,LINK_TRY
        call Proto_WaitCmd
        pop bc
        ret nc                           ; пришёл нужный ответ
        djnz .wait

        ; Ответа пока нет — спросить, жив ли ESP.
        ld a,CMD_PING
        call Proto_SendEmpty
        ld a,RESP_READY
        ld de,LINK_TRY
        call Proto_WaitCmd
        jr c,.silent
        ld a,LINK_SILENCE                ; отозвался — ждём дальше
        ld (LinkSilence),a
        jr .outer
.silent:
        ld hl,LinkSilence
        dec (hl)
        jr nz,.outer
        scf
        ret

; Попросить ESP закрыть listener. Ответ не критичен: плагин всё равно выходит.
Link_Stop:
        ld a,CMD_SMB_STOP
        call Proto_SendEmpty
        ld a,RESP_ACK
        ld de,LINK_TRY
        jp Proto_WaitCmd

; --- обслуживание файловых запросов -----------------------------------------

; Разобрать всё, что уже пришло от ESP, и ответить на файловые запросы.
; Доклад об ошибке (#EE) ничего не требует и просто пропускается.
Vfs_Serve:
        call Proto_Poll
        ret nc                           ; пакетов больше нет
        cp EVT_SMB_CLIENT
        jr z,SmbEvent_Client
        cp EVT_SMB_COMMAND
        jr z,SmbEvent_Command
        cp EVT_SMB_PROGRESS
        jr z,SmbEvent_Progress
        call Vfs_Dispatch
        jr Vfs_Serve

; Состояние SMB-клиента: 0=нет, 1=TCP подключён, 2=пользователь авторизован.
; Событие приходит только при реальном изменении, поэтому перерисовка окна не
; замедляет передачу блоков файла.
SmbEvent_Client:
        ld hl,(ProtoRxLen)
        ld a,h
        or l
        jr z,Vfs_Serve
        ld a,(ProtoBuf)
        or a
        ld hl,UiClientNone
        jr z,.show
        cp 2
        ld hl,UiClientConnected
        jr nz,.show
        ld hl,UiClientLogged
.show:
        call Ui_SetClient
        call Ui_Draw
        jr Vfs_Serve

; Последняя реальная операция SMB. Пароль в событие никогда не входит. Имена
; файлов приходят в UTF-8; перед выводом переводим их во внутреннюю кодировку
; формата Wild Commander.
SmbEvent_Command:
        ld hl,(ProtoRxLen)
        ld a,h
        or l
        jr z,Vfs_Serve
        ld de,ProtoBuf
        add hl,de
        ld (hl),0
        ld hl,ProtoBuf
        call Utf8_ToOemInPlace
        ld hl,ProtoBuf
        call Ui_SetLast
        call Ui_Draw
        jr Vfs_Serve

; Ход текущей передачи файла. ESP присылает готовую ASCII-строку не чаще
; четырёх раз в секунду и не повторяет одинаковый текст, поэтому здесь нет ни
; счётчиков, ни ограничения частоты: каждое пришедшее событие несёт новое
; значение и перерисовка окна оправдана. Пустое тело гасит поле — так снимается
; итог предыдущего файла при открытии следующего.
SmbEvent_Progress:
        ld hl,(ProtoRxLen)
        ld a,h
        or l
        ld hl,UiLastNone
        jr z,.show
        ld hl,(ProtoRxLen)
        ld de,ProtoBuf
        add hl,de
        ld (hl),0                        ; строка приходит без нуля на конце
        ld hl,ProtoBuf
.show:
        call Ui_SetProgress
        call Ui_Draw
        jr Vfs_Serve

; --- данные ------------------------------------------------------------------

LinkError:      db 0
LinkWant:       db 0
LinkSilence:    db 0                     ; счётчик пингов без ответа
; Тело SMB_START: порт, ресурс, имя компьютера, рабочая группа, логин и пароль.
; Каждая строка завершается нулём. Это единственное место с настройками SMB.
LinkSmbStart:
LinkSmbPort:    dw 445
                db "SD",0                ; имя общего ресурса
                db "ZX-Evo",0            ; имя NetBIOS
                db "WORKGROUP",0         ; рабочая группа Windows
                db "zx",0                ; логин
                db "zx",0                ; пароль
LinkSmbStartLen equ $-LinkSmbStart
LinkNetbios:    db 0                     ; 1, если ESP заняла UDP-порт 137
LinkIniMsg:     db "ini=",0
LinkNoReply:    db "no ESP reply",0
LinkNativePrefix: db "Native C++ ",0
LinkFailBuf:    ds 80
VfsCount:       dw 0                     ; сколько файловых запросов обслужено
VfsCountMsg:    db "VFS requests: ",0
VfsCountBuf:    ds 24
LocalIp:        ds 4                     ; IP, выданный роутером (его печатает окно)

        INCLUDE "ui.asm"
        INCLUDE "proto.asm"
        INCLUDE "vfs.asm"
        INCLUDE "fs.asm"
