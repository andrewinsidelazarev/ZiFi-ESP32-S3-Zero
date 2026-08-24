; Двоичный обмен с нативной прошивкой ESP32-S3.
;
; Формат пакета одинаков в обе стороны:
;   [SYNC=#5A][CMD][LEN_L][LEN_H][DATA…][CSUM]  — поля пакета по порядку
;   CSUM = XOR байта CMD со всеми последующими байтами (LEN_L, LEN_H, DATA).
;
; В старых текстовых протоколах асинхронные строки и двоичные данные легко
; рассинхронизировали Z80. Здесь длина известна заранее, экранирование не нужно,
; а CR/LF и нули внутри payload безопасны сами по себе.
;
; ESP не пишет в линию по своей инициативе: он либо отвечает на наш пакет, либо
; присылает файловый запрос, когда обслуживает FTP-клиента.

PROTO_SYNC      equ #5A
PROTO_MAX       equ 1024                ; предел payload, как и на стороне ESP
PROTO_ERR_TEXT  equ 64                  ; буфер текста доклада об ошибке

CMD_ECHO        equ #00
CMD_WIFI_INI    equ #03                 ; отдать zifi.ini целиком, разбирает ESP
CMD_PING        equ #04
CMD_GET_STEP    equ #05
CMD_FTP_START   equ #06
CMD_FTP_STOP    equ #07
CMD_UPDATE_START equ #08
CMD_UPDATE_STOP equ #09
CMD_SMB_START   equ #0B
CMD_SMB_STOP    equ #0C
CMD_ONLINE_UPDATE equ #0D              ; GitHub SHA-256 + firmware.bin -> OTA
CMD_ONLINE_UPDATE_CHECK equ #0E        ; только версия и SHA из manifest
CMD_SYS_RESET   equ #0F
CMD_SYS_INFO    equ #02
CMD_NET_IPCONFIG equ #21               ; текущие IP, маска, шлюз, DNS
CMD_NET_NTP     equ #22                ; точное время; ESP уже учла time: из ini
CMD_NET_PROXY_STATUS equ #23           ; статус HTTP-прокси (0=выкл, 1=вкл, 2=недоступен)
; TCP-клиент: подключиться, отправить, забрать порцию, закрыть. Данные тянет
; Z80 — приёмная очередь ZiFi всего 256 байт, и непрошеный поток её переполнит.
CMD_NET_OPEN    equ #10                ; адрес,0,порт LE16 (без порта — 80)
CMD_NET_SEND    equ #11                ; байты как есть, обычно HTTP-запрос
CMD_NET_RECV    equ #12                ; сколько байт максимум, LE16
CMD_NET_CLOSE   equ #13
; Готовый запрос GET: ESP подключается, отправляет запрос и отбрасывает
; заголовок ответа. Z80 получает только тело — ни склейки текста запроса, ни
; посимвольного поиска CR/LF/CR/LF на нём больше нет.
CMD_NET_HTTP_GET equ #14               ; адрес,0,порт LE16,путь,0
CMD_NET_PING    equ #20                ; ICMP-эхо: имя узла с нулём на конце
EVT_FTP_CLIENT  equ #60
EVT_FTP_COMMAND equ #61
EVT_SMB_CLIENT  equ #62
EVT_SMB_COMMAND equ #63
EVT_SMB_PROGRESS equ #64               ; готовая строка хода передачи файла
EVT_ONLINE_UPDATE_PROGRESS equ #65     ; [этап][проценты]

RESP_SYS_INFO   equ #82
RESP_WIFI_INI   equ #83
RESP_GET_STEP   equ #85                 ; метка шага и текст последней ошибки
RESP_FTP_START  equ #86
RESP_FTP_STOP   equ #87
RESP_UPDATE_START equ #88
RESP_UPDATE_STOP equ #89
RESP_SMB_START  equ #8B
RESP_SMB_STOP   equ #8C
RESP_ONLINE_UPDATE equ #8D             ; [успех][SHA-256 32 байта]
RESP_ONLINE_UPDATE_CHECK equ #8E       ; [успех][relation][версия ASCII]
ONLINE_UPDATE_SAME      equ 0
ONLINE_UPDATE_NEWER     equ 1
ONLINE_UPDATE_OLDER     equ 2
ONLINE_UPDATE_DIFFERENT equ 3
RESP_NET_IPCONFIG equ #A0
RESP_NET_PING   equ #A1
RESP_NET_NTP    equ #A2                ; ГГГГММДДЧЧММСС, 14 символов ASCII
RESP_NET_PROXY_STATUS equ #A3          ; [status(1B)][endpoint(ASCII)]
RESP_NET_OPEN   equ #90
RESP_NET_SEND   equ #91
; Первый байт: 0 — порция (пустая значит «данных пока нет, спроси снова»),
; 1 — сервер закрыл соединение. Путать нельзя: файл оборвётся на середине.
RESP_NET_RECV   equ #92
RESP_NET_CLOSE  equ #93
; Признак, код ответа HTTP (LE16) и длина тела (LE32, ноль — сервер не назвал).
RESP_NET_HTTP_GET equ #94
RESP_READY      equ #F0
RESP_ERROR      equ #EE                 ; доклад ESP об исключении
RESP_ACK        equ #FE

; --- передача ---------------------------------------------------------------

; Отправить пакет: A=CMD, HL=данные, BC=длина.
; Разрушает A/BC/DE/HL. Тайм-аут очереди передачи обрабатывает ZiFi_PutChar.
Proto_Send:
        ld (ProtoCmd),a
        ld (ProtoPtr),hl
        ld (ProtoLen),bc
        xor a
        ld (ProtoCsum),a

        ld a,PROTO_SYNC                 ; SYNC в контрольную сумму не входит
        call ZiFi_PutChar
        ret c
        ld a,(ProtoCmd)
        call Proto_PutSum
        ret c
        ld a,(ProtoLen)
        call Proto_PutSum
        ret c
        ld a,(ProtoLen+1)
        call Proto_PutSum
        ret c

        ld hl,(ProtoPtr)
        ld bc,(ProtoLen)
.body:
        ld a,b
        or c
        jr z,.finish
        ld a,(hl)
        push hl
        push bc
        call Proto_PutSum
        pop bc
        pop hl
        ret c
        inc hl
        dec bc
        jr .body
.finish:
        ld a,(ProtoCsum)
        jp ZiFi_PutChar

; Отправить байт из A и учесть его в контрольной сумме.
Proto_PutSum:
        ld e,a
        ld a,(ProtoCsum)
        xor e
        ld (ProtoCsum),a
        ld a,e
        jp ZiFi_PutChar

; Отправить пакет без данных: A=CMD.
Proto_SendEmpty:
        ld hl,0
        ld bc,0
        jp Proto_Send

; --- приём ------------------------------------------------------------------

; Неблокирующий разбор: ZiFi читается через INIR-порции до #BF байт, после чего
; пакет собирается из RAM. Пакет отдаётся только целиком и только с верной
; контрольной суммой; битый или слишком длинный молча отбрасывается.
; Выход: CF=1 и A=CMD — пакет готов (payload в ProtoBuf, длина в ProtoRxLen);
;        CF=0 — данных пока недостаточно.
Proto_Poll:
        call Proto_GetByte
        ret nc                          ; очередь пуста — пакета нет
        ld e,a                          ; E = принятый байт
        ld a,(ProtoState)

        or a
        jr nz,.st1
        ld a,e                          ; ждём SYNC, мусор вне пакета пропускаем
        cp PROTO_SYNC
        jp nz,Proto_Poll
        ld a,1
        ld (ProtoState),a
        jp Proto_Poll

.st1:
        dec a
        jr nz,.st2
        ld a,e                          ; команда
        ld (ProtoRxCmd),a
        xor a
        ld (ProtoCsumTmp),a
        ld a,(ProtoRxCmd)
        ld (ProtoCsumTmp),a             ; сумма начинается с CMD
        ld a,2
        ld (ProtoState),a
        jp Proto_Poll

.st2:
        dec a
        jr nz,.st3
        ld a,e                          ; младший байт длины
        ld (ProtoRxLen),a
        call Proto_AddCsum
        ld a,3
        ld (ProtoState),a
        jp Proto_Poll

.st3:
        dec a
        jr nz,.st4
        ld a,e                          ; старший байт длины
        ld (ProtoRxLen+1),a
        call Proto_AddCsum
        ld hl,ProtoBuf
        ld (ProtoRxPtr),hl
        ld hl,0
        ld (ProtoRxCount),hl
        ; Длиннее буфера пакет быть не может: ESP держит тот же предел.
        ld hl,(ProtoRxLen)
        ld bc,PROTO_MAX
        or a
        sbc hl,bc
        jr z,.len_ok
        jr c,.len_ok
        call Proto_ResetRx              ; невозможная длина — пакет игнорируем
        jp Proto_Poll
.len_ok:
        ld hl,(ProtoRxLen)
        ld a,h
        or l
        ld a,4
        jr nz,.set
        ld a,5                          ; данных нет — сразу сумма
.set:
        ld (ProtoState),a
        jp Proto_Poll

.st4:
        dec a
        jr nz,.st5
        ld hl,(ProtoRxPtr)              ; байт данных
        ld (hl),e
        inc hl
        ld (ProtoRxPtr),hl
        ld a,e
        call Proto_AddCsum
        ld hl,(ProtoRxCount)
        inc hl
        ld (ProtoRxCount),hl
        ld bc,(ProtoRxLen)
        ld a,h
        cp b
        jp nz,Proto_Poll
        ld a,l
        cp c
        jp nz,Proto_Poll
        ld a,5
        ld (ProtoState),a
        jp Proto_Poll

.st5:
        ld a,(ProtoCsumTmp)             ; сравнить накопленную сумму
        ld d,a
        call Proto_ResetRx
        ld a,e
        cp d
        jp nz,Proto_Poll                ; битый пакет — ищем следующий SYNC
        ld a,(ProtoRxCmd)
        scf
        ret

; Вернуть следующий байт из локальной INIR-порции. Новое обращение к ZIFR
; выполняется только после исчерпания уже принятой порции.
; Выход: CF=1 и A=байт либо CF=0, если очередь ZiFi пуста.
Proto_GetByte:
        ld a,(ProtoBurstLeft)
        or a
        jr nz,.have
        ld hl,ProtoBurstBuf
        call ZiFi_ReadBurst
        or a
        ret z
        ld (ProtoBurstLeft),a
        ld hl,ProtoBurstBuf
        ld (ProtoBurstPtr),hl
.have:
        ld hl,(ProtoBurstPtr)
        ld a,(hl)
        inc hl
        ld (ProtoBurstPtr),hl
        ld hl,ProtoBurstLeft
        dec (hl)
        scf
        ret

; Учесть байт A в накопленной контрольной сумме приёма.
Proto_AddCsum:
        ld hl,ProtoCsumTmp
        xor (hl)
        ld (hl),a
        ret

; Сбросить состояние приёмника.
Proto_ResetRx:
        xor a
        ld (ProtoState),a
        ld hl,0
        ld (ProtoRxCount),hl
        ret

; --- ожидание конкретного ответа --------------------------------------------

; Ждать пакет с командой A не дольше DE попыток опроса.
; Доклад об ошибке (#EE) сохраняется в ProtoErr и не считается ответом.
; Выход: CF=0 — пакет получен, CF=1 — тайм-аут.
Proto_WaitCmd:
        ld (ProtoWant),a
.loop:
        push de
        call Proto_Poll
        pop de
        jr nc,.idle
        ld hl,ProtoWant
        cp (hl)
        jr z,.got
        cp RESP_ERROR
        jr nz,.idle
        ld a,1
        ld (ProtoErr),a
        call Proto_SaveErr              ; сохранить текст: 0x83 затрёт ProtoBuf
.idle:
        dec de
        ld a,d
        or e
        jr nz,.loop
        scf
        ret
.got:
        or a
        ret

; Скопировать payload доклада об ошибке в отдельный буфер с нулём на конце.
Proto_SaveErr:
        ld hl,ProtoBuf
        ld de,ProtoErrText
        ld bc,(ProtoRxLen)
        ; Длину обязательно ограничиваем: ESP вправе прислать доклад длиной до
        ; PROTO_MAX, а буфер здесь 64 байта. Без ограничения хвост затирал
        ; ProtoBurstBuf и ProtoBuf, то есть приёмные буферы самого протокола —
        ; обмен разрушался прямо во время разбора пакета.
        ld a,b
        or a
        jr nz,.clamp
        ld a,c
        cp PROTO_ERR_TEXT-1
        jr c,.check
.clamp:
        ld bc,PROTO_ERR_TEXT-1
.check:
        ld a,b
        or c
        jr z,.term
.copy:
        ld a,(hl)
        ld (de),a
        inc hl
        inc de
        dec bc
        ld a,b
        or c
        jr nz,.copy
.term:
        xor a
        ld (de),a
        ret

; Запросить статус HTTP-прокси.
; Возвращает: CF=0 если ответ получен (A=статус: 0-выкл, 1-вкл, 2-недоступен;
; DE=указатель на строку адреса ProtoBuf+1 с нулём); CF=1 при тайм-ауте.
Net_ProxyStatus:
        ld hl,0
        ld bc,0
        ld a,CMD_NET_PROXY_STATUS
        call Proto_Send
        ld de,10000                     ; 10 000 итераций опроса (достаточно для ответа ESP)
        ld a,RESP_NET_PROXY_STATUS
        call Proto_WaitCmd
        ret c
        ld hl,(ProtoRxLen)
        ld a,h
        or l
        scf
        ret z
        ld bc,ProtoBuf
        add hl,bc
        ld (hl),0                       ; гарантируем завершающий ноль
        ld a,(ProtoBuf)                 ; статус (0, 1, 2)
        ld de,ProtoBuf+1                ; адрес "ip:port"
        or a
        ret

; --- данные -----------------------------------------------------------------

ProtoCmd:       db 0
ProtoPtr:       dw 0
ProtoLen:       dw 0
ProtoCsum:      db 0

ProtoState:     db 0                    ; 0..5 — состояние приёмника
ProtoRxCmd:     db 0
ProtoRxLen:     dw 0
ProtoRxPtr:     dw 0
ProtoRxCount:   dw 0
ProtoCsumTmp:   db 0
ProtoWant:      db 0
ProtoErr:       db 0                    ; 1 — ESP присылал доклад об ошибке
ProtoBurstPtr:  dw ProtoBurstBuf
ProtoBurstLeft: db 0
ProtoErrText:   ds PROTO_ERR_TEXT       ; сохранённый текст последнего доклада
ProtoBurstBuf:  ds #BF                  ; одна безопасная INIR-порция ZiFi
; Приёмник хранит не больше PROTO_MAX байтов. Ещё один байт зарезервирован под
; завершающий ноль: текстовый пакет длиной ровно 1024 байта тогда заканчивается
; внутри буфера, а не портит первую команду расположенного следом VFS-кода.
ProtoBuf:       ds PROTO_MAX+1          ; payload и безопасный текстовый ноль
