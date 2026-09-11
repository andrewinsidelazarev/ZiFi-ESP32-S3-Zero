; Получение погоды от прошивки ESP32-S3 через двоичный протокол ZiFi.
;
; Последовательность как у NTP-плагина: найти ZiFi, дождаться READY, отдать
; /zifi/zifi.ini командой WIFI_INI (сеть и индекс места разбирает ESP), затем
; WEATHER_GET. Ответ RESP_WEATHER — готовая запись REC_SIZE байт: название
; места в CP866, текущая погода, восход и закат, шесть дневных записей.
; Разбор JSON, геокодирование индекса и часовые пояса остаются на ESP; Z80
; только показывает числа. При любой неудаче заставка продолжает работать как
; часы и календарь, а вместо погоды пишет причину (StatusText).
;
; Пакеты протокола (proto.asm): [#5A][команда][длина LE16][данные][XOR].
; Ответы ESP приходят не сразу, поэтому ожидание идёт по кадрам: каждый кадр
; обновляются часы и проверяется клавиатура, чтобы заставку можно было
; закрыть даже пока ESP думает.

; CMD_WEATHER_GET (#24) и RESP_WEATHER (#A4) объявлены в общем proto.asm.
; Payload у команды нет: ESP каждый раз заново берёт прогноз, а координаты
; индекса помнит сама.
WEATHER_PING_TRIES equ 4
FRAMES_PING     equ 25                  ; 0,5 с на READY
FRAMES_ACK      equ 100                 ; 2 с на подтверждение команды
FRAMES_WIFI     equ 750                 ; 15 с на подключение к Wi-Fi
; ESP сама повторяет неудачные HTTP-запросы (503, обрыв): новые попытки
; начинает только в первые 30 секунд, но последняя может длиться ещё ~35.
; Ждём с запасом — часы при этом идут, клавиша закрывает заставку сразу.
FRAMES_WEATHER  equ 4500                ; 90 с на ответ ESP (50 кадров в секунду)

; Формат записи (см. tools/design.py, weather_parse.hpp). Числа — младшим
; байтом вперёд (LE), как привычно для Z80.
REC_VERSION     equ 1
REC_STATUS      equ 0                   ; 1 — данные есть
REC_VER         equ 1                   ; версия формата
REC_PLACE       equ 2                   ; название места, CP866, ноль в конце
REC_PLACE_LEN   equ 24
REC_TEMP        equ 26                  ; температура, знаковый байт
REC_CODE        equ 27                  ; код погоды WMO
REC_IS_DAY      equ 28                  ; 1 — день, 0 — ночь
REC_WIND10      equ 29                  ; ветер км/ч x10, слово
REC_PRECIP10    equ 31                  ; осадки мм x10, слово
REC_PRESS       equ 33                  ; давление мм рт. ст., слово
REC_SUNRISE     equ 35                  ; восход: час, минута
REC_SUNSET      equ 37                  ; закат: час, минута
REC_DATA_TIME   equ 39                  ; время данных: час, минута
REC_DAY_COUNT   equ 41                  ; число дневных записей
REC_DAYS        equ 42                  ; дневные записи по DAY_SIZE байт
DAY_SIZE        equ 8
DAY_DAY         equ 0                   ; число месяца
DAY_MONTH       equ 1                   ; месяц 1..12
DAY_WDAY        equ 2                   ; день недели, 0 — понедельник
DAY_CODE        equ 3                   ; код погоды WMO
DAY_TMIN        equ 4                   ; минимум, знаковый
DAY_TMAX        equ 5                   ; максимум, знаковый
DAY_PRECIP10    equ 6                   ; осадки за сутки x10, слово
REC_MAX_DAYS    equ 6
REC_SIZE        equ REC_DAYS+DAY_SIZE*REC_MAX_DAYS

; Первый запрос: ZiFi, Wi-Fi и запись погоды.
; Выход: WeatherValid=1 и запись в WeatherRecord, иначе StatusText — причина.
; AbortFlag=1 означает, что во время ожидания нажали клавишу.
Weather_Fetch:
        xor a
        ld (FetchOk),a                  ; итог этой попытки — пока неудача
        ld (WeatherValid),a
        ld (AbortFlag),a
        ld (WifiReady),a
        ld (ProtoErr),a
        ld (ProtoErrText),a             ; текст доклада ESP — пустая строка
        ; Код и переменные плагина остаются в памяти между запусками заставки:
        ; приёмник пакетов, прерванный клавишей посреди ответа, начинаем с нуля.
        ld (ProtoBurstLeft),a
        call Proto_ResetRx
        ld hl,Str_NO_ZIFI
        ld (StatusText),hl              ; причина на случай неудачи
        call ZiFi_Init                  ; CF=1 — модуля нет
        ret c

        ; ESP после включения или сброса готова не сразу: PING до ответа READY
        ld b,WEATHER_PING_TRIES
.ping:
        push bc                         ; B — счётчик попыток
        ld a,CMD_PING
        call Proto_SendEmpty
        ld a,RESP_READY
        ld de,FRAMES_PING
        call Weather_Wait
        pop bc
        jr nc,.ready                    ; ответ пришёл
        ld a,(AbortFlag)
        or a
        ret nz                          ; нажали клавишу — выходим
        djnz .ping
        ret                             ; ESP молчит — остаёмся «ZiFi не найден»

.ready:
        ld hl,Str_NO_INI
        ld (StatusText),hl
        call Config_Load                ; CF=1 — /zifi/zifi.ini не прочитан
        jr nc,.ini_ok
        ld a,(ConfigError)
        cp 4                            ; 4 — файл длиннее 1024 байт
        ret nz
        ld hl,Str_INI_TOO_LONG
        ld (StatusText),hl
        ret
.ini_ok:
        ; zifi.ini целиком — в ESP: она сама найдёт сеть, пароль, страну и индекс
        ld hl,Str_NO_WIFI
        ld (StatusText),hl
        ld a,CMD_WIFI_INI
        ld hl,IniBuffer
        ld bc,(IniLength)
        call Proto_Send
        ld a,RESP_ACK
        ld de,FRAMES_ACK
        call Weather_Wait               ; ESP подтверждает приём команды
        ret c
        ld a,RESP_WIFI_INI
        ld de,FRAMES_WIFI
        call Weather_Wait               ; ...и потом сообщает, поднялась ли сеть
        ret c
        ld a,(ProtoBuf)                 ; [status][IPv4]: 0 — сеть не поднялась
        or a
        ret z
        ld a,1
        ld (WifiReady),a
        ; продолжение — общий запрос записи

; Запросить запись погоды (сеть уже поднята). Выход как у Weather_Fetch;
; при неудаче прежняя запись и WeatherValid не меняются.
Weather_Request:
        ld hl,Str_NO_WEATHER
        ld (StatusText),hl
        xor a
        ld (FetchOk),a
        ld (ProtoErr),a
        ld (ProtoErrText),a
        ld a,CMD_WEATHER_GET
        call Proto_SendEmpty
        ld a,RESP_ACK
        ld de,FRAMES_ACK
        call Weather_Wait
        ret c
        ld a,RESP_WEATHER
        ld de,FRAMES_WEATHER
        call Weather_Wait               ; ESP ходит в интернет — ждём дольше
        ret c
        ; запись принимается только целиком, со статусом 1 и нашей версией
        ld hl,(ProtoRxLen)
        ld de,REC_SIZE
        or a
        sbc hl,de
        ret c                           ; короче записи — это ответ об ошибке
        ld a,(ProtoBuf+REC_STATUS)
        cp 1
        ret nz
        ld a,(ProtoBuf+REC_VER)
        cp REC_VERSION
        ret nz                          ; чужая версия формата — не показываем
        ld hl,ProtoBuf
        ld de,WeatherRecord
        ld bc,REC_SIZE
        ldir                            ; копия: ProtoBuf затрёт следующий пакет
        ld a,1
        ld (WeatherValid),a
        ld (FetchOk),a                  ; эта попытка удалась
        ret

; Ждать пакет с командой A не дольше DE кадров. Доклад об ошибке #EE
; сохраняется в ProtoErrText и ответом не считается. Каждый кадр обновляются
; часы (Saver_Tick) и проверяется клавиатура.
; Выход: CF=0 — пакет в ProtoBuf; CF=1 — тайм-аут либо нажата клавиша
; (AbortFlag=1).
Weather_Wait:
        ld (ProtoWant),a                ; какую команду ждём
.frame:
        push de                         ; DE — сколько кадров ещё ждать
        call Saver_Frame
.poll:
        call Proto_Poll                 ; CF=1 и A=CMD — пакет собран
        jr nc,.idle                     ; байтов больше нет — до следующего кадра
        ld hl,ProtoWant
        cp (hl)
        jr z,.got
        cp RESP_ERROR
        jr nz,.poll                     ; чужой пакет — пропустить
        ld a,1
        ld (ProtoErr),a
        call Proto_SaveErr              ; текст ошибки ESP — для экрана
        jr .poll
.idle:
        call Keys_Any
        jr nz,.abort
        call Saver_Tick                 ; часы идут, пока ждём
        pop de
        dec de
        ld a,d
        or e
        jr nz,.frame
        scf                             ; время вышло
        ret
.got:
        pop de
        or a                            ; CF=0 — пакет получен
        ret
.abort:
        ld a,1
        ld (AbortFlag),a
        pop de                          ; снять сохранённый DE со стека
        scf
        ret

WeatherValid:   db 0                    ; 1 — в WeatherRecord настоящие данные
FetchOk:        db 0                    ; 1 — последняя попытка принесла погоду
WifiReady:      db 0                    ; 1 — ESP подключилась к Wi-Fi
AbortFlag:      db 0                    ; 1 — ожидание прервано клавишей
StatusText:     dw Str_NO_ZIFI          ; причина, если погоды нет
WeatherRecord:  ds REC_SIZE             ; последняя принятая запись погоды
