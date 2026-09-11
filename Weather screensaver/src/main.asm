; Заставка Wild Commander «часы, календарь и погода» через ZiFi ESP32-S3.
;
; Плагин типа #02: Wild Commander запускает его сам по таймеру бездействия
; (ScreenSaver=N минут в wc.ini) или из меню плагинов F10. Экран рисуется в
; режиме TS-Conf 360x288 на 256 цветов в первом видеобуфере WC, время берётся
; из часов Mr.Gluk, а погоду по индексу из /zifi/zifi.ini получает ESP32-S3
; командой WEATHER_GET и отдаёт готовой двоичной записью (см. weather.asm).
;
; Структура файла .WMF: заголовок (512 байт), страница кода (#8000..#BFFF),
; затем три страницы данных по 16 КиБ: шрифты, малые иконки и большие иконки
; погоды. Страницы данных подключаются к окну #0000 функцией WC MNG0_PL.
; Все четыре страницы WC загружает в память один раз при старте и держит там
; до перезагрузки — поэтому переменные плагина сохраняют значения между
; запусками заставки, и об этом надо помнить (см. Weather_Fetch).
;
; Как читать исходник: main.asm — точка входа; video.asm — режим и страницы
; TS-Conf; gfx.asm — заливки, панели, иконки, глифы; text.asm — вывод строк;
; screen.asm — раскладка экрана. Общее с заставкой для VDAC2 лежит в
; ../shared/weather: saver.asm — главный цикл, weather.asm — обмен с ESP,
; rtc.asm — часы, calendar.asm — дни недели, fmt.asm — числа, wc.asm —
; переходники API WC. Файлы *.inc в src генерирует tools/gen_assets.py из
; tools/design.py.

        DEVICE ZXSPECTRUM128
        ; zifi.ini читается целиком, до 1024 байт, как в Online Update: ключи
        ; country:/zip: обычно дописывают в конец файла, а пример zifi.ini с
        ; комментариями уже длиннее одного сектора (511 байт прежнего предела).
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

        ; общие с заставкой для VDAC2 модули из ../shared/weather
        INCLUDE "saver.asm"             ; главный цикл
        INCLUDE "weather.asm"           ; обмен с ESP, таблица WMO
        INCLUDE "rtc.asm"               ; часы Mr.Gluk
        INCLUDE "calendar.asm"          ; дни недели, соседние даты
        INCLUDE "fmt.asm"               ; числа и строки в TextBuf
        INCLUDE "wc.asm"                ; переходники API WC, клавиатура
        ; экран TS-Conf — только в этой заставке
        INCLUDE "video.asm"
        INCLUDE "gfx.asm"
        INCLUDE "text.asm"
        INCLUDE "screen.asm"
        INCLUDE "config.asm"            ; общие модули ZiFi из ../shared/z80
        INCLUDE "proto.asm"
        INCLUDE "zifi_uart.asm"
        INCLUDE "assets.inc"            ; сгенерированные данные и таблицы
        INCLUDE "palette.inc"
        INCLUDE "strings.inc"

; --- переменные --------------------------------------------------------------------
LaunchReason:   db 0                    ; A при входе: #02 таймер, #03 меню F10
mainEnd:
        ; Код и данные страницы 0 обязаны поместиться в окно #8000..#BFFF.
        ASSERT mainEnd <= #C000, "plugin code exceeds the #8000 page"
        ENT

        ALIGN 512                       ; страницы данных — с границы сектора
page1:  INCBIN "../build/page1.bin"
page2:  INCBIN "../build/page2.bin"
page3:  INCBIN "../build/page3.bin"
        ASSERT $-page1 == 3*16384, "data pages must be exactly 16 KiB each"
endCode:
        SAVEBIN "../build/WEATHER.WMF",startCode,endCode-startCode
