; Главный цикл заставки.
;
; Порядок: часы и календарь рисуются сразу и показываются, пока ESP ходит в
; интернет; потом дорисовывается погода. Дальше каждый кадр (1/50 секунды):
; HALT, проверка клавиатуры, чтение часов. Смена секунды мигает двоеточием,
; смена минуты перерисовывает часы, смена дня — дату и календарь.
;
; Когда запрашивается погода:
;   - при запуске заставки;
;   - после удачного ответа — раз в час (REFETCH_MINUTES) и при смене дня
;     (утром строки прогноза сдвигаются);
;   - после неудачи (нет сети, сервер ответил 503 и т. п.) — повтор через
;     10, 20, 30, 60, 120 секунд, дальше каждые 5 минут, пока не получится
;     (RetryDelays). Перед повтором надпись об ошибке стирается и снова
;     пишется «Запрос погоды…».
; Любая клавиша закрывает заставку.
REFETCH_MINUTES equ 60

Saver_Run:
        call Keys_WaitRelease           ; Enter из меню F10 не должен закрыть нас
        call Video_Begin                ; палитра, сброс кэша страниц
        call Rtc_Open
        call Rtc_Read
        ; «показанные» значения — чтобы Saver_Tick замечал изменения
        ld a,(RtcSec)
        ld (ShownSec),a
        ld a,(RtcMin)
        ld (ShownMin),a
        ld a,(RtcDay)
        ld (ShownDay),a
        xor a
        ld (WeatherValid),a
        ld (WifiReady),a
        ld (RefetchDue),a
        ld (MinutesSinceFetch),a
        ld (RetryIndex),a
        ld h,a
        ld l,a
        ld (RetrySeconds),hl            ; повтор не запланирован
        ; кадр рисуется в невидимом пока банке видеостраниц
        call Screen_Static
        call Screen_Clock
        call Screen_Date
        call Screen_ForecastTitle
        call Screen_Calendar
        ld hl,Str_LOADING
        ld (StatusText),hl
        call Screen_Current             ; «Запрос погоды…», пока нет данных
        call Video_Show                 ; только теперь включаем графику
        call Weather_Fetch
        ld a,(AbortFlag)
        or a
        jr nz,.exit                     ; клавишу нажали, пока ждали ESP
        call Saver_Schedule             ; когда спросить погоду в следующий раз
        call Screen_Weather
.loop:
        call Saver_Frame                ; ждать следующий кадр
        call Keys_Any
        jr nz,.exit                     ; любая клавиша — выход
        call Saver_Tick                 ; часы, дата, календарь, отсчёт повтора
        ld a,(RefetchDue)
        or a
        jr z,.loop
        call Saver_Refetch              ; пора запросить погоду
        ld a,(AbortFlag)
        or a
        jr z,.loop
.exit:
        call Video_End
        ; Клавиша, закрывшая заставку, не должна дойти до панелей Wild
        ; Commander: иначе Enter открыл бы файл под курсором.
        jp Keys_WaitRelease

; Повторный запрос погоды. Если сеть при запуске не поднялась, путь полный:
; ZiFi, zifi.ini, Wi-Fi. Неудача не стирает уже показанную погоду:
; WeatherRecord меняется только при успешном ответе.
Saver_Refetch:
        xor a
        ld (RefetchDue),a
        ld (MinutesSinceFetch),a        ; следующий плановый запрос — через час
        ld a,(WeatherValid)
        or a
        jr nz,.keep_screen              ; погода на экране остаётся до ответа
        ; Погоды нет — на экране причина прошлой неудачи («…http 503»).
        ; Стираем её и пишем «Запрос погоды…», чтобы было видно новую попытку.
        ld (ProtoErrText),a             ; A = 0: пустой текст ошибки
        ld hl,Str_LOADING
        ld (StatusText),hl
        call Screen_Current
.keep_screen:
        ld a,(WifiReady)
        or a
        jr nz,.request
        call Weather_Fetch              ; сеть ещё не поднималась — полный путь
        jr .done
.request:
        call Weather_Request
.done:
        ld a,(AbortFlag)
        or a
        ret nz
        call Saver_Schedule
        jp Screen_Weather

; Запланировать следующий запрос по итогу последнего (FetchOk).
; Удача: повторы сбрасываются, дальше — раз в час. Неудача: через
; RetryDelays[RetryIndex] секунд; индекс растёт до последнего элемента
; таблицы и на нём остаётся — дальше повтор каждые 5 минут.
; Если ZiFi вовсе нет, повторять бессмысленно.
Saver_Schedule:
        ld a,(FetchOk)
        or a
        jr z,.failed
        xor a
        ld (RetryIndex),a
        ld h,a
        ld l,a
        ld (RetrySeconds),hl            ; 0 — повтор не нужен
        ret
.failed:
        ld a,(ZifiPresent)
        or a
        ret z                           ; модуля нет — ждать нечего
        ld a,(RetryIndex)
        ld l,a
        ld h,0
        add hl,hl                       ; два байта на элемент таблицы
        ld de,RetryDelays
        add hl,de
        ld e,(hl)
        inc hl
        ld d,(hl)
        ld (RetrySeconds),de            ; через сколько секунд повторить
        ld a,(RetryIndex)
        cp RETRY_STEPS-1
        ret nc                          ; последний шаг — дальше не растём
        inc a
        ld (RetryIndex),a
        ret

; Отсчёт до повтора: вызывается раз в секунду. Дошёл до нуля — пора.
Saver_RetryTick:
        ld hl,(RetrySeconds)
        ld a,h
        or l
        ret z                           ; повтор не запланирован
        dec hl
        ld (RetrySeconds),hl
        ld a,h
        or l
        ret nz
        ld a,1
        ld (RefetchDue),a
        ret

; Ждать следующий кадр. Единственный HALT плагина: прерывание WC приходит
; 50 раз в секунду, а машинный тест заменяет эту процедуру на RET и сам
; считает кадры. Байт-заполнитель перед меткой нужен тесту: эмулятор z80
; проверяет точку останова по адресу конца предыдущей команды, и метка сразу
; после JP срабатывала бы ложно.
        nop
Saver_Frame:
        halt                            ; процессор спит до прерывания
        ret

; Один кадр: часы прочитаны, изменившиеся части экрана перерисованы.
; Вызывается и из ожидания ответа ESP, поэтому сеть здесь не трогается.
Saver_Tick:
        call Rtc_Read
        ; минута проверяется первой: если между чтениями часов прошло ровно
        ; 60 секунд, секунды совпадут, а минута — нет
        ld a,(RtcMin)
        ld hl,ShownMin
        cp (hl)
        jr nz,.new_minute
        ld a,(RtcSec)
        ld hl,ShownSec
        cp (hl)
        ret z                           ; та же секунда — ничего не менялось
        ld (hl),a
        call Saver_RetryTick            ; новая секунда — отсчёт повтора
        jp Screen_Colon                 ; та же минута — только двоеточие
.new_minute:
        ld (hl),a                       ; HL всё ещё указывает на ShownMin
        ld a,(RtcSec)
        ld (ShownSec),a
        call Saver_RetryTick
        ld a,(MinutesSinceFetch)
        inc a
        ld (MinutesSinceFetch),a
        cp REFETCH_MINUTES
        jr c,.no_refetch                ; меньше часа с прошлого запроса
        ld a,1
        ld (RefetchDue),a
.no_refetch:
        call Screen_Clock               ; вместе с двоеточием нужной секунды
        ld a,(RtcDay)
        ld hl,ShownDay
        cp (hl)
        ret z
        ld (hl),a
        ld a,1
        ld (RefetchDue),a               ; новый день — сдвинулся и прогноз
        call Screen_Date
        jp Screen_Calendar

; Всё, что зависит от записи погоды.
Screen_Weather:
        call Screen_Location
        call Screen_Current
        jp Screen_Forecast

; Паузы перед повторами после неудачного запроса, в секундах.
RetryDelays:
        dw 10, 20, 30, 60, 120, 300
RETRY_STEPS     equ ($-RetryDelays)/2

ShownSec:       db #FF                  ; последние нарисованные секунда,
ShownMin:       db #FF                  ; минута
ShownDay:       db #FF                  ; и число месяца
RefetchDue:     db 0                    ; 1 — пора запросить погоду снова
MinutesSinceFetch: db 0                 ; минут с последнего запроса погоды
RetryIndex:     db 0                    ; номер следующей паузы в RetryDelays
RetrySeconds:   dw 0                    ; секунд до повтора, 0 — не нужен
