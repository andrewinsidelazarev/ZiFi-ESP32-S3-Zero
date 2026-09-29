; Точка входа плагина. PLUGIN обязан быть первым байтом по адресу #8000.
;
; ZiFi WC Update — аналог sfc /scannow для Wild Commander Improved. Всю
; проверку ведёт ESP: берёт с GitHub список каталога exe с git-SHA каждого
; файла, читает те же файлы с SD файловыми запросами к плагину, сверяет SHA и
; присылает строки списка. Плагин показывает список, даёт отметить файлы и
; отдаёт ESP номера отмеченных.
;
; Обновление одного файла на ESP: скачать и сверить SHA (плохое скачивание —
; скачать снова) → записать на SD во временный WCUPD.TMP → прочитать обратно и
; сверить SHA (не сошлось — записать снова) → поставить копию на место
; старого файла (FILEX MOVE с заменой; без него — через WCUPD.OLD) → ещё раз
; прочитать с SD и сверить. Старый файл уходит только тогда, когда рядом
; лежит копия, уже совпавшая с GitHub. WCUPD.TMP открывается на запись, лишь
; если имя свободно: остаток прошлой замены не удаляется (VFS_WRITE_NEW_ONLY).
;
; wc.ini не перезаписывается никогда: это защищённый путь команды START.
;
; IX при входе указывает на служебную структуру Wild Commander; поле IX+29 —
; устройство активной панели, его надо забрать до того, как IX займут окна.

WCU_MAX         equ 96                  ; строк списка, как kMaxFiles у ESP
WCU_ENTRY_SIZE  equ 64                  ; байт на строку в таблице
WCU_TABLE       equ #C000               ; таблица — страница 2 плагина в #C000
WCU_PAGE_TABLE  equ 2
WCU_NAME_MAX    equ 54                  ; путь в событии ENTRY: 63-9 байт

; Строка таблицы.
E_STATUS        equ 0                   ; состояние от ESP (WCU_ST_*)
E_FLAGS         equ 1                   ; флаги от ESP и отметка пользователя
E_LOCAL         equ 2                   ; размер на SD, LE24
E_REMOTE        equ 5                   ; размер на GitHub, LE24
E_NAMELEN       equ 8
E_NAME          equ 9                   ; путь в кодировке WC

WCU_F_REMOTE    equ %00000001           ; файл есть на GitHub
WCU_F_LOCAL     equ %00000010           ; файл есть на SD
WCU_F_KEPT      equ %00000100           ; защищён: имеющийся не заменяется
WCU_F_CAN       equ %00001000           ; можно обновить
WCU_F_AUTO      equ %00010000           ; исполняемый: отмечается клавишей A
WCU_F_ESP       equ %00011111           ; всё, что присылает ESP
WCU_F_MARK      equ %10000000           ; отмечен пользователем
WCU_B_CAN       equ 3
WCU_B_AUTO      equ 4
WCU_B_MARK      equ 7

; Состояния строк (WcStatus прошивки).
WCU_ST_UNKNOWN  equ 0
WCU_ST_SAME     equ 1
WCU_ST_DIFF     equ 2
WCU_ST_NEW      equ 3
WCU_ST_LOCAL    equ 4
WCU_ST_KEPT     equ 5
WCU_ST_KEPTDIFF equ 6
WCU_ST_READERR  equ 7
WCU_ST_UPDATED  equ 8
WCU_ST_FAILED   equ 9

; Этапы (WcPhase прошивки).
WCU_PH_GITHUB   equ 1
WCU_PH_LOCAL    equ 2
WCU_PH_CHECK    equ 3
WCU_PH_READY    equ 4
WCU_PH_APPLY    equ 5
WCU_PH_ERROR    equ #FF
WCU_PH_SYNC     equ 6                   ; метка WCU_SYNC: дальше весь список

PLUGIN:
        push ix
        ld a,(ix+29)
        ld (ConfigPanelDevice),a
        ; Страница плагина живёт в памяти между запусками, поэтому всё
        ; состояние обнуляется здесь, а не полагается на начальные db.
        ld hl,WcuState
        ld de,WcuState+1
        ld bc,WcuStateEnd-WcuState-1
        ld (hl),0
        ldir
        xor a
        ld (VfsChanged),a
        ld (VfsFilexKnown),a             ; возможности FILEX спросить заново
        ld (ProtoErr),a
        ld (ProtoBurstLeft),a
        call Proto_ResetRx
        ld a,WCU_SYNCS
        ld (WcuSyncsLeft),a
        ld a,1
        call WC_INT_PL                   ; WC не должен перерисовывать часы под окном
        ; 14 МГц на время работы, как по умолчанию в WC (CPU_FREQ=2). У ZiFi
        ; нет управления потоком: на 115200 байт приходит раз в 87 мкс, а
        ; разбор протокола и строки списка на 3,5 МГц дольше — сплошной поток
        ; событий переполнил бы очередь. При выходе частота возвращается.
        ld bc,#0002
        call WC_TURBOPL
        call Vfs_Crc16Init               ; таблицы CRC — до первого файлового обмена
        call Wcu_ClearTable
        call WC_GEDPL
        call Ui_Open

        ld hl,UiStageConfig
        call Ui_StatusNow
        call Config_Load                 ; найти и прочитать zifi.ini
        jp c,.config_error

        ld hl,UiStageZifi
        call Ui_StatusNow
        call Link_Start                  ; ZiFi + готовность ESP + Wi-Fi
        jp c,.network_error

.start:
        ld hl,UiStageStart
        call Ui_StatusNow
        ld hl,(WC_FRAMES)
        ld (WcuLastPacket),hl
        call Wcu_Start
        ; STOP при выходе нужен, если ESP команду START узнала: пришёл ACK
        ; или сам ответ A5 (ACK мог потеряться). Старая прошивка ответила бы
        ; только «unsupported».
        push af
        ld a,(WcuReqAcked)
        ld (WcuStarted),a
        pop af
        jr nc,.started
        ld a,(WcuExit)
        or a
        jp z,.start_error
        jp .exit                        ; Esc во время ожидания START
.started:
        ld a,1
        ld (WcuStarted),a

; Главный цикл. Сначала разбирается всё, что пришло от ESP: файловые запросы
; ждут ответа, а приёмная очередь ZiFi — всего 256 байт без управления
; потоком, её хватает на 22 мс. Клавиши — раз в кадр, как у самого WC.
; Рисуется окно по одной строке и только когда линия молчит: вывод строки
; занимает единицы миллисекунд, и очередь не переполнится, даже если ESP как
; раз начнёт окно записи в 16 КиБ.
.loop:
        ei
        call Wcu_Serve
        ld a,(WcuExit)
        or a
        jr nz,.exit
        ld a,(WC_FRAMES)
        ld hl,UiKeyFrame
        cp (hl)
        jr z,.draw
        ld (hl),a
        call Wcu_Watch
        call Ui_KeysFrame
        jr .loop
.draw:
        call Link_RxEmpty
        jr nz,.loop
        call Ui_DrawStep
        jr nz,.loop
        call Link_WaitRx
        jr .loop

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
        ; LinkError: 1 — нет ZiFi, 2 — ESP молчит, 3 — Wi-Fi.
        ld a,(LinkError)
        cp 1
        ld hl,UiErrorNoZifi
        jr z,.fatal
        cp 2
        ld hl,UiErrorFirmware
        jr z,.fatal
        call Link_BuildFail             ; HL -> "ERROR: Wi-Fi ini=NN <причина>"
        jr .fatal

.start_error:
        ; Причину присылает ESP пакетом #EE: у старой прошивки это
        ; «unsupported:25» — команд обновлятора в ней нет.
        ld hl,UiErrorStart
        ld a,(ProtoErr)
        or a
        jr z,.fatal
        ld hl,UiErrorWord
        ld (UiStatusPrefix),hl
        ld hl,ProtoErrText

.fatal:
        call Ui_StatusNow
.fatal_wait:
        ei
        halt
        call WC_ESC
        jr z,.fatal_wait

.exit:
        ld a,(WcuStarted)
        or a
        jr z,.close
        ld hl,0
        ld (UiStatusPrefix),hl
        ld (UiStatusSuffix),hl
        ld hl,UiStageStopping
        call Ui_StatusNow
        ; ESP закончит шаг и уберёт WCUPD.TMP. Ответ [0] — задача ещё не
        ; завершилась за 20 с: просить снова, обслуживая файловые запросы,
        ; иначе она осталась бы работать без плагина.
        ld b,WCU_STOPS
.stop:
        push bc
        call Wcu_Stop
        pop bc
        jr c,.close                     ; ESP не отвечает вовсе
        or a
        jr nz,.close                    ; остановлена
        djnz .stop
.close:
        ld ix,WcuWindow
        call WC_RRESB
        call WC_GEDPL                    ; вернуть основной экран WC
        ld b,#FF
        call WC_TURBOPL                  ; частота Z80 — как настроена в WC
        ; Обновлённые файлы — изменения на диске: код 3 просит WC перечитать
        ; обе панели, сохранив активную. Без записи возвращаем 0.
        ld a,(VfsChanged)
        or a
        jr z,.leave
        ld a,3
.leave:
        pop ix
        ret

; --- запуск связи -------------------------------------------------------------

; Единица опроса очереди ZiFi для ожиданий до START. Длительность ими не
; меряется: живость ESP проверяет пинг (см. Link_WaitResult).
LINK_TRY        equ 12000
LINK_PROBE      equ 10                   ; проб между пингами живости
LINK_SILENCE    equ 15                   ; столько пингов без READY = ESP мёртв

; ZiFi, перезапуск ESP, READY и Wi-Fi по zifi.ini. Выход: CF=1 и LinkError.
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
        ; ESP перезапускается перед работой, как у FTP: обновлятору нужны
        ; большие буферы (список GitHub, файл целиком), а куча после прошлого
        ; сеанса раздроблена. Готовность устанавливает цикл PING ниже.
        ld a,CMD_SYS_RESET
        call Proto_SendEmpty
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
        ld hl,UiStageWifi
        call Ui_StatusNow
        ; zifi.ini отдаётся целиком: SSID и пароль разбирает ESP.
        ld a,CMD_WIFI_INI
        ld hl,IniBuffer
        ld bc,(IniLength)
        call Proto_Send
        ld a,RESP_ACK
        ld de,LINK_TRY
        call Proto_WaitCmd
        ld a,RESP_WIFI_INI
        call Link_WaitResult
        jr c,.wifi_fail
        ld a,(ProtoBuf)
        or a
        jr z,.wifi_fail
        ld a,1
        ld (WcuLinkUp),a
        or a                             ; CF=0: Wi-Fi подключён
        ret
.wifi_fail:
        ld a,3
        ld (LinkError),a
        scf
        ret

; Строка отказа Wi-Fi: сколько байт zifi.ini прочитано и что ответил ESP.
; Выход: HL -> строка с нулём.
Link_BuildFail:
        ld hl,LinkIniMsg               ; "ERROR: Wi-Fi ini="
        ld de,LinkFailBuf
        call CopyZNoTerm
        ld hl,(IniLength)
        call U16_ToDec
        ld a,' '
        ld (de),a
        inc de
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

; Ждать ответ A сколько потребуется: выход по самому ответу либо по молчанию
; ESP на пинги. Только до START — чужие пакеты здесь пропускаются.
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
        ret nc
        djnz .wait
        ld a,CMD_PING
        call Proto_SendEmpty
        ld a,RESP_READY
        ld de,LINK_TRY
        call Proto_WaitCmd
        jr c,.silent
        ld a,LINK_SILENCE
        ld (LinkSilence),a
        jr .outer
.silent:
        ld hl,LinkSilence
        dec (hl)
        jr nz,.outer
        scf
        ret

; Ждать данных от ESP, но не дольше конца текущего кадра: выход по первому
; байту в очереди ZiFi или по смене кадрового таймера WC. Предел витков — на
; случай, если прерываний нет вовсе.
LINK_SPIN       equ 8192

Link_WaitRx:
        ld a,(WC_FRAMES)
        ld e,a
        ld hl,LINK_SPIN
        ld bc,ZIFI_RX_COUNT
.spin:
        in a,(c)
        or a
        ret nz                          ; байт пришёл — разбирать сразу
        ld a,(WC_FRAMES)
        cp e
        ret nz                          ; кадр сменился
        dec hl
        ld a,h
        or l
        jr nz,.spin
        ret

; Z — ничего не ждёт разбора: ни остатка принятой порции, ни байтов в ZiFi.
Link_RxEmpty:
        ld a,(ProtoBurstLeft)
        or a
        ret nz
        ld bc,ZIFI_RX_COUNT
        in a,(c)
        or a
        ret

; --- команды обновлятора ------------------------------------------------------

; ESP теряет команду, пришедшую, пока её файловый клиент ждёт ответа Z80:
; тогда ACK не приходит, и команда уходит снова. ACK шлётся сразу по приёму,
; так что полсекунды без него — потеря, а не долгая работа. Ответ после ACK
; на START и STOP ждём дольше: ESP даёт прежней задаче до 20 с закончить шаг,
; и всё это время присылает файловые запросы — их обслуживаем. APPLY ESP
; только ставит в очередь и отвечает сразу.
WCU_ACK_FRAMES  equ 25                  ; 0,5 с без ACK — отправить снова
WCU_SENDS       equ 6                   ; всего попыток отправки
WCU_REPLY_FRAMES equ 2000               ; 40 с на ответ START и STOP после ACK
WCU_APPLY_FRAMES equ 250                ; 5 с на ответ APPLY после ACK
WCU_STOPS       equ 3                   ; сколько раз просить остановку
; Потерянное событие (а подтверждений у событий нет) лечится запросом SYNC:
; ESP выдаёт заново все строки и последнее состояние. Признаки: итог READY,
; а строк меньше, чем в нём указано, или в таблице дыра; неполный повтор;
; либо долгое молчание, пока ESP вроде бы занята. Потерь — не больше
; WCU_SYNCS за сеанс.
WCU_SYNCS       equ 5
WCU_QUIET_FRAMES equ 3000               ; 60 с без пакетов при занятой ESP
WCU_RESYNC_FRAMES equ 500               ; 10 с без итога после SYNC

; Отправить команду и дождаться ответа, обслуживая файловые запросы и события.
; Вход: A — команда, HL — данные, BC — длина, E — ожидаемый ответ;
; WcuReqReply — кадров на ответ после ACK; WcuReqEsc=1 — Esc (раз в кадр)
; прерывает ожидание: WcuExit=1, CF=1.
; Выход: CF=0, A — первый байт ответа (0 при пустом); CF=1 — ответа нет.
; WcuReqAcked — узнала ли ESP команду.
Wcu_Request:
        ld (WcuReqCmd),a
        ld (WcuReqPtr),hl
        ld (WcuReqLen),bc
        ld a,e
        ld (WcuReqWant),a
        ld a,WCU_SENDS
        ld (WcuReqSends),a
        xor a
        ld (WcuReqAcked),a
.send:
        ld a,(WcuReqCmd)
        ld hl,(WcuReqPtr)
        ld bc,(WcuReqLen)
        call Proto_Send
        ld hl,(WC_FRAMES)
        ld (WcuReqSince),hl
.poll:
        call Proto_Poll
        jr nc,.idle
        cp RESP_ACK
        jr z,.ack
        ld hl,WcuReqWant
        cp (hl)
        jr z,.answer
        call Wcu_Packet
        jr .poll
.ack:
        ld a,(WcuReqAcked)
        or a
        jr nz,.poll                     ; ACK повторной отправки — срок не сдвигать
        inc a
        ld (WcuReqAcked),a
        ld hl,(WC_FRAMES)
        ld (WcuReqSince),hl             ; ответ отсчитывается от ACK
        jr .poll
.answer:
        ld hl,(ProtoRxLen)
        ld a,h
        or l
        jr z,.empty
        ld a,(ProtoBuf)
        or a                            ; CF=0
        ret
.empty:
        xor a                           ; CF=0, A=0
        ret
.idle:
        call Link_WaitRx
        ld a,(WcuReqEsc)
        or a
        jr z,.timers
        ld a,(WC_FRAMES)
        ld hl,WcuReqKeyFrame
        cp (hl)
        jr z,.timers
        ld (hl),a
        call WC_ESC
        jr nz,.escape
.timers:
        ld hl,(WC_FRAMES)
        ld de,(WcuReqSince)
        or a
        sbc hl,de                       ; прошло кадров
        ld a,(WcuReqAcked)
        or a
        jr nz,.acked
        ld de,WCU_ACK_FRAMES
        sbc hl,de                       ; CF уже 0 после OR
        jr c,.poll
        ld hl,WcuReqSends
        dec (hl)
        jr nz,.send
        scf
        ret
.acked:
        ld de,(WcuReqReply)
        sbc hl,de
        jr c,.poll
        scf
        ret
.escape:
        ld a,1
        ld (WcuExit),a
        scf
        ret

; START: ESP поднимает задачу проверки и сразу начинает слать события.
; Выход: CF=0 — задача запущена. Доклад #EE этапа Wi-Fi к отказу START
; отношения не имеет — сбрасывается.
Wcu_Start:
        xor a
        ld (ProtoErr),a
        ld hl,WCU_REPLY_FRAMES
        ld (WcuReqReply),hl
        inc a
        ld (WcuReqEsc),a                ; Esc — выход и во время запуска
        ld a,CMD_WCU_START
        ld hl,WcuStartPayload
        ld bc,WcuStartPayloadLen
        ld e,RESP_WCU_START
        call Wcu_Request
        ret c
        or a
        ret nz
        scf
        ret

; APPLY: HL — номера строк, BC — их число. Выход: CF=0 — список принят.
Wcu_Apply:
        push hl
        ld hl,WCU_APPLY_FRAMES
        ld (WcuReqReply),hl
        pop hl
        ld a,1
        ld (WcuReqEsc),a
        ld a,CMD_WCU_APPLY
        ld e,RESP_WCU_APPLY
        call Wcu_Request
        ret c
        or a
        ret nz
        scf
        ret

; STOP: ESP прерывает работу после текущего шага. Выход: CF=0 и A=1 —
; задача завершилась; A=0 — ещё работает; CF=1 — ответа нет.
Wcu_Stop:
        ld hl,WCU_REPLY_FRAMES
        ld (WcuReqReply),hl
        xor a
        ld (WcuReqEsc),a                ; выход уже идёт
        ld a,CMD_WCU_STOP
        ld hl,0
        ld bc,0
        ld e,RESP_WCU_STOP
        jp Wcu_Request

; Раз в кадр из главного цикла, вне любого ожидания команды.
;
; SYNC — просьба повторить список: ESP шлёт метку (этап 6, «всего» — число
; строк), все строки по порядку номеров и последнее состояние. Повтор полон,
; только если после метки пришли строки 0..N-1 подряд и затем итог (READY
; или ERROR); неполный — просьба повторяется. SYNC уходит без ожидания
; ответа: ожидание снимает итог полного повтора, а нет его 10 с после
; последнего пакета — просьба повторяется.
;
; SYNC просится: после итога каждого APPLY (строки меняются на месте, и
; потерянную обновлённую строку по дыре не найти; вне лимита на потери); при
; дыре или нехватке строк в итоге проверки; при неполном повторе; при 60 с
; тишины, пока ESP занята (проверка или APPLY). Потерь — не больше
; WCU_SYNCS; лимит исчерпан — ошибка на экране: список мог устареть.
Wcu_Watch:
        ld a,(WcuRefreshWanted)
        or a
        jr nz,.refresh
        ld a,(WcuSyncWanted)
        or a
        jr nz,.lost
        ld de,WCU_RESYNC_FRAMES
        ld a,(WcuSyncPending)
        or a
        jr nz,.quiet
        ld de,WCU_QUIET_FRAMES
        ld a,(WcuBusy)
        or a
        jr nz,.quiet                    ; APPLY отправлен, итога нет
        ld a,(WcuPhase)
        cp WCU_PH_READY
        ret z
        cp WCU_PH_ERROR
        ret z
.quiet:
        ld hl,(WC_FRAMES)
        push de
        ld de,(WcuLastPacket)
        or a
        sbc hl,de                       ; прошло кадров (по модулю 65536)
        pop de
        or a
        sbc hl,de
        ret c
.lost:
        xor a
        ld (WcuSyncWanted),a
        ld hl,WcuSyncsLeft
        ld a,(hl)
        or a
        jr z,.stale
        dec (hl)
        jr .send
.refresh:
        xor a
        ld (WcuRefreshWanted),a
.send:
        ld hl,(WC_FRAMES)
        ld (WcuLastPacket),hl           ; тишина — от отправки
        ld a,1
        ld (WcuSyncPending),a
        xor a
        ld (WcuSyncOn),a                ; строки прежнего повтора не в счёт
        ld a,CMD_WCU_SYNC
        ld hl,0
        ld bc,0
        jp Proto_Send                   ; ответ A8 не ждём: его пропустит Vfs_Dispatch
.stale:
        ; Потерь больше лимита: ждать нечего, список на экране мог устареть.
        xor a
        ld (WcuSyncPending),a
        ld hl,0
        ld (UiStatusPrefix),hl
        ld (UiStatusSuffix),hl
        ld hl,UiErrorStale
        jp Ui_SetStatus

; Итог READY: все ли строки на месте. Число файлов у ESP — поле «всего».
Wcu_CheckList:
        ld a,(WcuTot+1)
        or a
        jr nz,.lost
        ld a,(WcuTot)
        ld hl,WcuCount
        cp (hl)
        jr nz,.lost
        or a
        ret z
        ld b,a
        call Wcu_MapTable
        ld hl,WCU_TABLE+E_STATUS
        ld de,WCU_ENTRY_SIZE
.row:
        ld a,(hl)
        or a
        jr z,.lost                      ; дыра: строка не пришла
        add hl,de
        djnz .row
        ret
.lost:
        ld a,1
        ld (WcuSyncWanted),a
        ret

; --- разбор пакетов ESP -------------------------------------------------------

; Разобрать всё, что уже пришло.
Wcu_Serve:
        call Proto_Poll
        ret nc
        call Wcu_Packet
        jr Wcu_Serve

; A — команда пакета, payload в ProtoBuf, длина в ProtoRxLen.
Wcu_Packet:
        ld hl,(WC_FRAMES)
        ld (WcuLastPacket),hl
        cp EVT_WCU_STATE
        jp z,Wcu_State
        cp EVT_WCU_ENTRY
        jp z,Wcu_Entry
        cp RESP_ERROR
        jr z,.error
        cp VFS_MOVE_RENAME
        jp z,Vfs_MoveRename
        cp VFS_RENAME
        jp z,Vfs_Rename
        ; Файловый запрос; запоздалые ответы на повторённые команды, ACK и
        ; прочее Vfs_Dispatch сам пропускает.
        jp Vfs_Dispatch
.error:
        ld a,1
        ld (ProtoErr),a
        jp Proto_SaveErr

; Состояние: [этап][текущий LE16][всего LE16][процент][текст].
Wcu_State:
        ld hl,(ProtoRxLen)
        ld de,6
        or a
        sbc hl,de
        ret c
        ld a,(ProtoBuf)
        cp WCU_PH_SYNC
        jr nz,.state
        ; Метка повтора списка: дальше строки 0..N-1 и последнее состояние.
        ; Этап, текст и полосу она не меняет. Строк больше 255 в таблице не
        ; бывает — такой повтор заведомо неполон.
        ld a,1
        ld (WcuSyncOn),a
        xor a
        ld (WcuSyncRows),a
        ld a,(ProtoBuf+4)
        ld (WcuSyncBad),a
        ld a,(ProtoBuf+3)
        ld (WcuSyncTotal),a
        ret
.state:
        ld hl,(ProtoRxLen)
        ld de,ProtoBuf
        add hl,de
        ld (hl),0                       ; текст без нуля — дописать
        ld a,(ProtoBuf)
        ld (WcuPhase),a
        ld hl,(ProtoBuf+1)
        ld (WcuCur),hl
        ld hl,(ProtoBuf+3)
        ld (WcuTot),hl
        ld a,(ProtoBuf+5)
        cp 101
        jr c,.percent
        ld a,100
.percent:
        ld (WcuPercent),a
        ld hl,ProtoBuf+6
        call Utf8_ToOemInPlace          ; путь в тексте — в кодировку WC
        ; Ошибка — с пометкой; итог после обновления — с напоминанием, что
        ; новые файлы WC возьмёт только после перезапуска.
        ld hl,0
        ld (UiStatusSuffix),hl
        ld a,(WcuPhase)
        cp WCU_PH_ERROR
        jr nz,.no_error
        ld hl,UiErrorWord
.no_error:
        ld (UiStatusPrefix),hl
        cp WCU_PH_READY
        jr nz,.text
        ld a,(WcuApplied)
        or a
        jr z,.text
        ld hl,UiRestartHint
        ld (UiStatusSuffix),hl
.text:
        ld hl,ProtoBuf+6
        call Ui_SetStatus
        ld a,1
        ld (UiDirtyBar),a
        ld a,(WcuPhase)
        cp WCU_PH_APPLY
        jr nz,.not_apply
        ; Идёт APPLY (даже если его ответ A6 потерялся): после итога —
        ; повтор списка.
        ld a,1
        ld (WcuRefreshAfter),a
        ret
.not_apply:
        cp WCU_PH_ERROR
        jr z,.final
        cp WCU_PH_READY
        ret nz
.final:
        ; Итог (READY или ERROR) снимает занятость: снова можно отмечать и
        ; запускать обновление.
        xor a
        ld (WcuBusy),a
        ld hl,WcuSyncOn
        or (hl)
        jr z,.plain
        ld (hl),0
        ; Итог повтора: весь ли список пришёл — строки 0..N-1 подряд.
        ld a,(WcuSyncBad)
        or a
        jr nz,.incomplete
        ld a,(WcuSyncRows)
        ld hl,WcuSyncTotal
        cp (hl)
        jr nz,.incomplete
        xor a
        ld (WcuSyncPending),a           ; список свежий: ждать нечего,
        ld (WcuRefreshAfter),a          ; и после APPLY повторять не нужно
        ret
.incomplete:
        ld a,(WcuSyncPending)
        or a
        ret z                           ; повтор не просили — не наш
        ld (WcuSyncWanted),a            ; просить снова
        ret
.plain:
        ; Итог обычного хода: проверки или APPLY.
        ld hl,WcuRefreshAfter
        or (hl)
        jr z,.check
        ld (hl),0
        ld (WcuRefreshWanted),a         ; итог обновления: список — заново
        ret
.check:
        ld a,(WcuSyncPending)
        or a
        ret nz                          ; итог повтора ещё впереди
        ld a,(WcuPhase)
        cp WCU_PH_READY
        ret nz
        jp Wcu_CheckList

; Строка списка: [номер][состояние][флаги][SD LE24][GitHub LE24][путь].
Wcu_Entry:
        ld hl,(ProtoRxLen)
        ld de,9
        or a
        sbc hl,de
        ret c
        ld a,(ProtoBuf)
        cp WCU_MAX
        ret nc
        ld (WcuIndex),a
        ld hl,(ProtoRxLen)
        ld de,ProtoBuf
        add hl,de
        ld (hl),0                       ; путь без нуля — дописать
        ld hl,ProtoBuf+9
        call Utf8_ToOemInPlace

        call Wcu_MapTable
        ld a,(WcuIndex)
        call Wcu_EntryAddr              ; HL -> строка таблицы
        ; Отметка пользователя переживает обновление строки, пока файл ещё
        ; можно обновить: например, неудачный останется отмеченным для повтора.
        inc hl
        ld a,(ProtoBuf+2)
        and WCU_F_ESP
        bit WCU_B_CAN,a
        jr z,.flags
        ld c,a
        ld a,(hl)
        and WCU_F_MARK
        or c
.flags:
        ld (hl),a
        dec hl
        ld a,(ProtoBuf+1)
        ld (hl),a                       ; состояние
        inc hl
        inc hl
        ex de,hl                        ; DE -> E_LOCAL
        ld hl,ProtoBuf+3
        ld bc,6
        ldir                            ; размеры SD и GitHub
        ; DE -> E_NAMELEN: длина и сам путь. Длиннее WCU_NAME_MAX — хвост:
        ; имя файла в конце пути важнее.
        push de
        ld hl,ProtoBuf+9
        ld bc,0
.measure:
        ld a,(hl)
        or a
        jr z,.measured
        inc hl
        inc c
        jr .measure
.measured:
        ld hl,ProtoBuf+9
        ld a,c
        sub WCU_NAME_MAX
        jr c,.from
        ld c,a
        add hl,bc                       ; B=0: пропустить лишнее начало
.from:
        pop de
        push de
        inc de
        ld b,0
.name:
        ld a,(hl)
        or a
        jr z,.named
        cp ' '
        jr nc,.put
        ld a,'.'                        ; управляющие байты на экран не пускать
.put:
        ld (de),a
        inc hl
        inc de
        inc b
        ld a,b
        cp WCU_NAME_MAX
        jr c,.name
.named:
        pop hl
        ld (hl),b                       ; E_NAMELEN

        ; Строка повтора: номера должны идти подряд с нуля.
        ld a,(WcuSyncOn)
        or a
        jr z,.counted
        ld a,(WcuIndex)
        ld hl,WcuSyncRows
        cp (hl)
        jr nz,.gap
        inc (hl)
        jr .counted
.gap:
        ld a,1
        ld (WcuSyncBad),a
.counted:
        ld hl,WcuCount
        ld a,(WcuIndex)
        cp (hl)
        jr c,.dirty
        inc a
        ld (hl),a                       ; список вырос
        ld a,1
        ld (UiDirtyScroll),a
.dirty:
        ld a,(WcuIndex)
        jp Ui_DirtyIndex

; Подключить таблицу в #C000. Любой вывод на экран потом сам вернёт TXT, а
; vfs.asm перед записью — свою страницу 1.
Wcu_MapTable:
        ld a,WCU_PAGE_TABLE
        jp WC_MNGC_PL

; A — номер строки. Выход: HL -> её место в таблице (#C000 + A*64).
Wcu_EntryAddr:
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        ld de,WCU_TABLE
        add hl,de
        ret

; Обнулить таблицу: страница переживает запуски и хранит строки прошлого.
Wcu_ClearTable:
        call Wcu_MapTable
        ld hl,WCU_TABLE
        ld de,WCU_TABLE+1
        ld bc,WCU_MAX*WCU_ENTRY_SIZE-1
        ld (hl),0
        ldir
        ret

; --- данные ------------------------------------------------------------------

; Тело START: репозиторий, ветка, каталог на GitHub, затем защищённые пути
; (их имеющиеся копии не заменяются) и пустая строка в конце.
WcuStartPayload:
        db "andrewinsidelazarev/Wild-Commander-Improved",0
        db "main",0
        db "exe",0
        db "WC/wc.ini",0
        db 0
WcuStartPayloadLen equ $-WcuStartPayload

LinkIniMsg:     db "ERROR: Wi-Fi ini=",0
LinkNoReply:    db "no ESP reply",0
VfsChanged:     db 0                     ; 1 — диск менялся: при выходе WC перечитает панели

; Всё изменяемое состояние одним блоком: PLUGIN обнуляет его целиком.
WcuState:
LinkError:      db 0
LinkWant:       db 0
LinkSilence:    db 0
WcuLinkUp:      db 0                     ; ESP и Wi-Fi готовы
WcuStarted:     db 0                     ; START узнан ESP: при выходе нужен STOP
WcuPhase:       db 0
WcuBusy:        db 0                     ; APPLY отправлен, итога ещё нет
WcuApplied:     db 0                     ; был принят APPLY
WcuExit:        db 0
WcuPercent:     db 0
WcuCur:         dw 0
WcuTot:         dw 0
WcuCount:       db 0                     ; строк в таблице
WcuIndex:       db 0
WcuReqCmd:      db 0
WcuReqWant:     db 0
WcuReqPtr:      dw 0
WcuReqLen:      dw 0
WcuReqSends:    db 0
WcuReqAcked:    db 0
WcuReqSince:    dw 0
WcuLastPacket:  dw 0                     ; кадр последнего пакета ESP
WcuSyncWanted:  db 0
WcuSyncsLeft:   db 0
WcuSyncPending: db 0                     ; SYNC ушёл, итога ещё нет
WcuRefreshAfter: db 0                    ; APPLY принят: после итога — SYNC
WcuRefreshWanted: db 0
WcuSyncOn:      db 0                     ; идёт повтор: метка была, итога нет
WcuSyncRows:    db 0                     ; строк повтора подряд с нуля
WcuSyncTotal:   db 0                     ; строк в повторе по метке
WcuSyncBad:     db 0                     ; в повторе дыра или лишнее
WcuReqReply:    dw 0                     ; кадров на ответ команды после ACK
WcuReqEsc:      db 0                     ; Esc прерывает ожидание ответа
WcuReqKeyFrame: db 0                     ; кадр последнего опроса Esc
UiKeyFrame:     db 0
UiKeyWait:      db 0                     ; кадров отложенная клавиша ждёт перерисовку
UiNavKey:       db 0                     ; отложенная клавиша (UI_KEY_*), 0 — нет
UiCursor:       db 0
UiTop:          db 0
UiRows:         db 0
UiDirtyStatus:  db 0
UiDirtyBar:     db 0
UiDirtyScroll:  db 0
UiRowDirty:     ds UI_MAX_ROWS
UiStatusPrefix: dw 0
UiStatusSuffix: dw 0
WcuStateEnd:
WcuApplyList:   ds WCU_MAX

        INCLUDE "ui.asm"
        INCLUDE "proto.asm"
        INCLUDE "vfs.asm"
        INCLUDE "fs.asm"
        INCLUDE "vfs_rename.asm"
