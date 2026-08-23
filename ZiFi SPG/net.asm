; Сетевой слой ZiFi поверх двоичного протокола Native C++ прошивки.
;
; Оригинал разговаривал с ESP текстовыми AT-командами и разбирал асинхронные
; ответы: «+IPD,<len>:», «CLOSED», приглашение '>'. Здесь тот же обмен идёт
; пакетами фиксированного формата — длина известна заранее, двоичные данные не
; нуждаются в экранировании, а CR/LF и нули внутри тела безопасны сами по себе.
;
; Вся грязная работа вынесена на ESP. Она сама подключается, собирает текст
; запроса и отбрасывает заголовок ответа, а Z80 остаётся только складывать тело
; по страницам:
;   AT+CIPSTART, CIPSEND и разбор заголовка -> NET_HTTP_GET #14
;   AT+CIPRECVDATA                          -> NET_RECV     #12
;   AT+CIPCLOSE                             -> NET_CLOSE    #13
;   AT+CWJAP_CUR                            -> WIFI_INI     #03 (ini целиком)
;
; Данные по-прежнему тянет Спектрум, а не проталкивает ESP: приёмная очередь
; ZiFi всего 256 байт, и поток, который никто не запрашивал, её переполнит.
; Ровно поэтому оригинал и переходил на AT+CIPRECVMODE=1.



; Ответ NET_RECV содержит байт признака конца и данные. Логический протокол
; допускает 1024 байта, но аппаратная очередь ZiFi заметно меньше: кадр
; 1023+1+заголовок+сумма мог теряться целиком. Просим 255 байт, поэтому payload
; равен 256, а полный wire-кадр — 261 байту; это проверенный безопасный класс
; порций и не требует непрерывно принимать килобайт без паузы.
NET_CHUNK           equ 255

; Единицы тайм-аута те же, что у ZiFi_SetTimeout: реальное число опросов равно
; значению, сдвинутому на 11 разрядов. На 14 МГц 40 — примерно одна секунда.
; Это только защита Z80 от полностью молчащей прошивки. Состояние TCP по
; истечению времени не угадывается: успех даёт только фактический RESP_*.
NET_WAIT_PING       equ 40              ; ответ локальной прошивки: до 1 с
; DNS и TCP-подключение идут на ESP синхронно, и первый запрос после входа в
; сеть бывает медленным: разрешение имени секунды, соединение с далёким узлом
; ещё столько же. Прежние четыре секунды кончались раньше, чем ESP успевала
; ответить, и Z80 сообщал «NET_OPEN failed» на живом соединении.
; HTTPS дополнительно выполняет TLS handshake, проверку сертификата и до пяти
; редиректов. Это только сторож Z80: недоступный сервер ESP отвергнет раньше.
NET_WAIT_OPEN       equ 7200            ; около трёх минут на всю цепочку
; У прошивки свой предел ожидания ICMP — три секунды, и она отвечает всегда.
; Прежние двенадцать секунд тишины на стороне Z80 выглядели зависанием.
NET_WAIT_PINGHOST   equ 120             ; около 3,5 с: прошивка отвечает за 1,5
NET_WAIT_SEND       equ 40              ; передача локальному ESP: до 1 с
NET_WAIT_RECV       equ 48              ; ожидание ответа ESP на один recv
NET_WAIT_CLOSE      equ 20              ; закрытие: до 0,5 с
NET_WAIT_WIFI       equ 480             ; connect_wifi на ESP ограничен 10 с
NET_WAIT_NTP        equ 300             ; запрос времени: DNS плюс обмен по UDP
NET_WAIT_DIAG       equ 20

; Ждать пакет с командой A. Тайм-аут задаётся заранее через ZiFi_SetTimeout.
; ACK #FE пропускается, а доклад об ошибке #EE сохраняется: следующий пакет
; статуса всё ещё нужен, но текст исключения нельзя терять — его покажет
; встроенная консоль ZiFi.
; Нажатая кнопка отмены уводит управление тем же путём, что и в оригинале:
; wifi_cancel_download_ex восстанавливает стек и возвращается в главный цикл.
; Выход: CF=0 — пакет принят (payload в ProtoBuf, длина в ProtoRxLen),
;        CF=1 — не дождались.
Net_WaitCmd:
        ld (NetWant),a
; Время и кнопка отмены проверяются на КАЖДОМ витке, а не только когда очередь
; пуста. Иначе поток чужих байтов — мусор после перезагрузки ESP или хвост
; прерванной закачки — держал Z80 в разборе без ограничения по времени: снаружи
; это выглядело зависанием, потому что до печати причины дело не доходило.
.loop:
        ld a,(wifi_cancel_download+1)
        or a
        jp nz,wifi_cancel_download_ex
        call ZiFi_CheckTimeout          ; CF=1 — время ещё есть
        jr nc,.expired
        call Proto_Poll
        jr nc,.loop
        ld hl,NetWant
        cp (hl)
        jr z,.got
        cp RESP_ERROR
        jr nz,.loop
        ld a,1
        ld (ProtoErr),a
        call Proto_SaveErr
        jr .loop
.expired:
        scf
        ret
.got:
        or a
        ret

; Отправить пакет A с данными HL длиной BC и очистить прошлую ошибку.
; Свой предел ожидания задаёт каждая операция сразу после отправки: TCP OPEN,
; локальные SEND/RECV и необязательный CLOSE не должны зависать одинаково.
Net_Request:
        push af
        xor a
        ld (ProtoErr),a
        ld (ProtoErrText),a
        pop af
        jp Proto_Send

; Проверить, что прошивка жива: PING должен вернуть READY.
; Выход: CF=0 — ответила.
Net_Ping:
        ld hl,0
        ld bc,0
        ld a,CMD_PING
        call Net_Request
        ld de,NET_WAIT_PING
        call ZiFi_SetTimeout
        ld a,RESP_READY
        jp Net_WaitCmd

; Забрать у ESP сохранённый этап и текст последней ошибки. Это резервный путь:
; основной пакет #EE мог потеряться вместе с ответом операции, а GET_STEP
; маленький и выполняется уже после завершения сетевого обработчика.
; Выход: CF=1 — ESP не ответил; CF=0 и ProtoErr=1 — текст в ProtoErrText;
;        CF=0 и ProtoErr=0 — ответ был, но сохранённого текста нет.
Net_GetStep:
        ld a,#FF
        ld (NetStepByte),a
        ld hl,0
        ld bc,0
        ld a,CMD_GET_STEP
        call Net_Request
        ld de,NET_WAIT_DIAG
        call ZiFi_SetTimeout
        ld a,RESP_GET_STEP
        call Net_WaitCmd
        ret c
        ld hl,(ProtoRxLen)
        ld a,h
        or l
        jr z,.empty
        ; Первый байт — числовая метка шага. Раньше она отбрасывалась, а это
        ; самое информативное поле: ноль означает, что состояние потеряно, то
        ; есть плата перезагрузилась, а не обработчик доложил об ошибке.
        ld a,(ProtoBuf)
        ld (NetStepByte),a
        dec hl
        ld (ProtoRxLen),hl
        ld a,h
        or l
        jr z,.empty
        ld hl,ProtoBuf+1
        ld de,ProtoBuf
        ld bc,(ProtoRxLen)
        ldir
        call Proto_SaveErr
        ld a,1
        ld (ProtoErr),a
        or a
        ret
.empty:
        xor a
        ld (ProtoErr),a
        ld (ProtoErrText),a
        ret

; Подключиться к точке доступа. Имя и пароль подготовил parse_ini в виде
; ssid,0,пароль,0 — тот же разбор zifi.ini, что и раньше, только без кавычек
; AT-команды.
; Выход: CF=0 — подключились.
Net_WifiConnect:
        ; zifi.ini уходит на ESP ЦЕЛИКОМ, ключи разбирает прошивка (wifi_ini.py).
        ; Ровно так это делает плагин NTP. Разбирать файл на Z80 незачем: у ESP
        ; уже есть разборщик всех пар «ключ: значение», и второй, отдельный
        ; способ подключения означал бы две разные истины про одну настройку.
        ; Файл лежит в странице download_page, подключённой к окну #C000.
        ld a,download_page
        call set_page3
        ld hl,#c000
        ld bc,(ini_length)
        ld a,CMD_WIFI_INI
        call Proto_Send
        ld de,NET_WAIT_WIFI
        call ZiFi_SetTimeout
        ld a,RESP_WIFI_INI
        call Net_WaitCmd
        ret c
        ld a,(ProtoBuf)                 ; 1 — сеть поднялась, 0 — нет
        or a
        ret nz
        scf
        ret

; Запросить файл по HTTP/HTTPS. Адрес сервера лежит в cmd_conn2site_adr, путь —
; в request_path, порт — в request_port; всё это положил parse_url.
; Всю грязь делает ESP: собирает текст запроса, отправляет и отбрасывает
; заголовок ответа. Z80 получает только тело файла.
; Выход: CF=1 — ответа нет; CF=0 и A=0 — отказ; CF=0 и A=1 — пошло тело,
;        код ответа HTTP в NetHttpCode, названная сервером длина в NetHttpLen.
Net_HttpGet:
        ld hl,cmd_conn2site_adr
        ld de,httpget_payload
        ld bc,0
        call Net_CopyZ                  ; адрес сервера вместе с нулём
        ld hl,(request_port)
        ld a,l
        ld (de),a
        inc de
        ld a,h
        ld (de),a
        inc de
        inc bc
        inc bc
        ld hl,request_path
        call Net_CopyZ                  ; путь вместе с нулём
        ; Длину запоминаем: пакет длиннее PROTO_MAX прошивка молча отбрасывает,
        ; и снаружи это неотличимо от «ESP не ответила». Число печатает консоль.
        ld (NetPayloadLen),bc
        ld hl,httpget_payload
        ld a,CMD_NET_HTTP_GET
        call Net_Request
        ld de,NET_WAIT_OPEN             ; внутри DNS, подключение и запрос
        call ZiFi_SetTimeout
        ld a,RESP_NET_HTTP_GET
        call Net_WaitCmd
        ret c
        ld hl,(ProtoBuf+1)
        ld (NetHttpCode),hl
        ld hl,(ProtoBuf+3)
        ld (NetHttpLen),hl
        ld hl,(ProtoBuf+5)
        ld (NetHttpLen+2),hl
        ld a,(ProtoBuf)
        or a
        ret

; Скопировать строку из HL в DE вместе с завершающим нулём, добавив длину к BC.
Net_CopyZ:
        ld a,(hl)
        ld (de),a
        inc hl
        inc de
        inc bc
        or a
        jr nz,Net_CopyZ
        ret

; Запросить очередную порцию тела файла. Заголовок HTTP до Z80 не доходит: его
; отбросила ESP.
; Выход: CF=1 — ответа не дождались (обрыв);
;        CF=0 и A=1 — данных больше не будет;
;        CF=0 и A=0 — в ProtoBuf+1 лежит NetChunkLen байтов, возможно ноль
;        (это «данных пока нет», а не конец файла).
Net_Recv:
        ld hl,NetChunkReq
        ld bc,2
        ld a,CMD_NET_RECV
        call Net_Request
        ld de,NET_WAIT_RECV
        call ZiFi_SetTimeout
        ld a,RESP_NET_RECV
        call Net_WaitCmd
        ret c
        ld a,(ProtoErr)
        or a
        jr z,.response_ok
        scf
        ret
.response_ok:
        ld hl,(ProtoRxLen)
        ld a,h
        or l
        jr z,.empty                     ; пакет без признака — считаем концом
        dec hl                          ; длина без байта признака
        ld (NetChunkLen),hl
        ld a,(ProtoBuf)
        or a
        ret
.empty:
        ld (NetChunkLen),hl
        ld a,1
        or a
        ret

; Один ICMP-отклик от узла, имя которого лежит по HL строкой с нулём. Ответ
; прошивки: признак плюс время в миллисекундах.
; Выход: CF=1 — ответа от прошивки нет; CF=0 и A=0 — узел не отозвался;
;        CF=0 и A=1 — время отклика в NetPingMs.
Net_PingHost:
        ld bc,0
.len:
        ld a,(hl)
        inc hl
        inc bc
        or a
        jr nz,.len                      ; ноль входит в тело: его ждёт разбор
        sbc hl,bc                       ; CF=0 после «or a» на нулевом байте
        ld a,CMD_NET_PING
        call Net_Request
        ld de,NET_WAIT_PINGHOST
        call ZiFi_SetTimeout
        ld a,RESP_NET_PING
        call Net_WaitCmd
        ret c
        ld hl,(ProtoBuf+1)
        ld (NetPingMs),hl
        ld a,(ProtoBuf)
        or a
        ret

; Точное время у прошивки: NET_NTP отдаёт ГГГГММДДЧЧММСС, 14 символов ASCII.
; Смещение из поля time: файла zifi.ini прошивка применила сама, поэтому на Z80
; прибавлять его второй раз нельзя. Неудачу она сообщает строкой из нулей.
; Выход: CF=0 — время в ProtoBuf.
Net_GetTime:
        ld hl,0
        ld bc,0
        ld a,CMD_NET_NTP
        call Net_Request
        ld de,NET_WAIT_NTP
        call ZiFi_SetTimeout
        ld a,RESP_NET_NTP
        call Net_WaitCmd
        ret c
        ld hl,(ProtoRxLen)
        ld a,l
        cp 14                           ; строка короче — верить нечему
        jr c,.bad
        ld a,h
        or a
        jr nz,.bad
        ld a,(ProtoBuf)                 ; год «00..» бывает только при отказе
        cp "0"
        jr nz,.good
        ld a,(ProtoBuf+1)
        cp "0"
        jr z,.bad
.good:
        or a
        ret
.bad:
        scf
        ret

; Закрыть соединение. Отсутствие ответа не считается ошибкой: файл уже принят,
; а сокет ESP закроет и сама.
Net_Close:
        ld hl,0
        ld bc,0
        ld a,CMD_NET_CLOSE
        call Net_Request
        ld de,NET_WAIT_CLOSE
        call ZiFi_SetTimeout
        ld a,RESP_NET_CLOSE
        jp Net_WaitCmd

NetWant:        db 0
NetChunkReq:    dw NET_CHUNK            ; сколько байт просим за раз
NetChunkLen:    dw 0                    ; сколько пришло в последней порции
NetPingMs:      dw 0                    ; время последнего ICMP-отклика
NetStepByte:    db #FF                  ; метка шага из GET_STEP, #FF — не спрашивали
NetPayloadLen:  dw 0                    ; длина тела последнего NET_HTTP_GET
NetHttpCode:    dw 0                    ; код ответа HTTP
NetHttpLen:     ds 4                    ; длина тела, названная сервером
