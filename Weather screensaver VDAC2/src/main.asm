; Заставка Wild Commander «часы, календарь и погода» для платы VDAC2 (FT812).
;
; Плагин типа #02: Wild Commander запускает его сам по таймеру бездействия
; (ScreenSaver=N минут в wc.ini) или из меню плагинов F10. Экран — видеочип
; FT812 платы VDAC2 в режиме 1024x768@59; время берётся из часов Mr.Gluk, а
; погоду по индексу из /zifi/zifi.ini получает ESP32-S3 командой WEATHER_GET и
; отдаёт готовой двоичной записью. Без платы VDAC2 заставка сразу выходит.
;
; Структура файла .WMF: заголовок (512 байт), страница кода (#8000..#BFFF),
; затем четыре страницы данных по 16 КиБ: таблицы шрифтов, неизменная часть
; кадра и сжатые zlib шрифты и иконки, которые при запуске распаковывает в
; свою память сам FT812. Страницы данных подключаются к окну #0000 функцией
; WC MNG0_PL. WC загружает все страницы один раз и держит их до перезагрузки
; — переменные плагина сохраняют значения между запусками заставки.
;
; Как читать исходник: main.asm — точка входа; ft812.asm — обмен с FT812 по
; SPI, запуск чипа, очередь сопроцессора, загрузка ресурсов; video.asm —
; включение экрана FT812 и возврат экрана WC; screen.asm — сборка кадра.
; Общее с заставкой TS-Conf лежит в ../shared/weather: saver.asm — главный
; цикл, weather.asm — обмен с ESP, rtc.asm — часы, calendar.asm — дни недели,
; fmt.asm — числа, wc.asm — переходники API WC. Файлы *.inc в src генерирует
; tools/gen_assets.py из tools/design.py.

        DEVICE ZXSPECTRUM128
        ; zifi.ini читается целиком, до 1024 байт (см. TS-Conf-версию)
        DEFINE CONFIG_FULL_INI
        INCLUDE "wc_api.inc"
        INCLUDE "layout.inc"

startCode:
        ORG #0000
        INCLUDE "wc_header.inc"

        ALIGN 512                       ; код начинается после сектора заголовка
        DISP #8000                      ; код работает с адреса #8000
mainStart:

; Точка входа плагина: CALL #8000 из Wild Commander.
; Вход: A — причина запуска (#02 таймер заставки, #03 меню F10).
; Выход: A=0 — обычный выход, панели перечитывать не нужно.
Start:
        ld (LaunchReason),a
        push ix                         ; структура панели WC нужна ему после нас
        call Saver_Run
        pop ix
        xor a
        ret

        ; общие с заставкой TS-Conf модули из ../shared/weather
        INCLUDE "saver.asm"             ; главный цикл
        INCLUDE "weather.asm"           ; обмен с ESP, таблица WMO
        INCLUDE "rtc.asm"               ; часы Mr.Gluk
        INCLUDE "calendar.asm"          ; дни недели, соседние даты
        INCLUDE "fmt.asm"               ; числа и строки в TextBuf
        INCLUDE "wc.asm"                ; переходники API WC, клавиатура
        INCLUDE "assets.inc"            ; сгенерированные таблицы и константы
        ; экран FT812 — только в этой заставке
        INCLUDE "ft812.asm"
        INCLUDE "video.asm"
        INCLUDE "screen.asm"
        INCLUDE "config.asm"            ; общие модули ZiFi из ../shared/z80
        INCLUDE "proto.asm"
        INCLUDE "zifi_uart.asm"
        INCLUDE "strings.inc"

; --- переменные --------------------------------------------------------------------
LaunchReason:   db 0                    ; A при входе: #02 таймер, #03 меню F10
mainEnd:
        ; Код и данные страницы 0 обязаны поместиться в окно #8000..#BFFF.
        ASSERT mainEnd <= #C000, "plugin code exceeds the #8000 page"
        ENT

        ALIGN 512                       ; страницы данных — с границы сектора
endCode:
        ; Заголовок и код. Четыре страницы данных по 16 КиБ вместе с кодом
        ; не помещаются в 64 КиБ адресов ассемблера, поэтому их дописывает
        ; в конец файла build.bat (copy /b): page1.bin .. page4.bin.
        SAVEBIN "../build/WEATHER2.WMF",startCode,endCode-startCode
