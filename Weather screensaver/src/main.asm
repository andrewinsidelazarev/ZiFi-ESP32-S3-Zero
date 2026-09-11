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
; Как читать исходник: main.asm — точка входа и общие переходники API WC;
; saver.asm — главный цикл; video.asm — режим и страницы TS-Conf; gfx.asm —
; заливки, панели, иконки, глифы; text.asm — вывод строк и форматирование
; чисел; screen.asm — раскладка экрана; rtc.asm — часы; calendar.asm — дни
; недели; weather.asm — обмен с ESP. Файлы *.inc в src генерирует
; tools/gen_assets.py из tools/design.py.

        DEVICE ZXSPECTRUM128
        ; zifi.ini читается целиком, до 1024 байт, как в Online Update: ключи
        ; country:/zip: обычно дописывают в конец файла, а пример zifi.ini с
        ; комментариями уже длиннее одного сектора (511 байт прежнего предела).
        DEFINE CONFIG_FULL_INI
        INCLUDE "wc_api.inc"
        INCLUDE "layout.inc"

; Функции WC, которых нет в общем wc_api.inc.
FN_ANYK         equ 45                  ; нажата ли любая клавиша (NZ — да)
FN_USPO         equ 46                  ; ждать отпускания всех клавиш
FN_MNGV_PL      equ 64                  ; выбрать банк видеостраниц (0 — текст WC)
FN_MNGCVPL      equ 65                  ; видеостраница A' -> окно #C000
FN_GVMOD        equ 66                  ; задать видеорежим плагина
FN_MNG0_PL      equ 78                  ; страница плагина A' -> окно #0000

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

        INCLUDE "saver.asm"
        INCLUDE "video.asm"
        INCLUDE "gfx.asm"
        INCLUDE "text.asm"
        INCLUDE "screen.asm"
        INCLUDE "rtc.asm"
        INCLUDE "calendar.asm"
        INCLUDE "weather.asm"
        INCLUDE "config.asm"            ; общие модули ZiFi из ../shared/z80
        INCLUDE "proto.asm"
        INCLUDE "zifi_uart.asm"
        INCLUDE "assets.inc"            ; сгенерированные данные и таблицы
        INCLUDE "palette.inc"
        INCLUDE "strings.inc"

; --- переходники API Wild Commander ------------------------------------------------
; Общий вход WC_API (#6006) получает номер функции в A. Там, где A уже занят
; параметром, значение передаётся через альтернативный AF (EX AF,AF').
; Эти пять переходников нужны общему config.asm (чтение zifi.ini).
WC_STREAM:
        ld a,FN_STREAM
        jp WC_API
WC_FENTRY:
        ld a,FN_FENTRY
        jp WC_API
WC_GFILE:
        ld a,FN_GFILE
        jp WC_API
WC_GDIR:
        ld a,FN_GDIR
        jp WC_API
WC_LOAD512:
        ld a,FN_LOAD512
        jp WC_API

; Нажата ли клавиша. Выход: NZ — нажата. Сохраняет HL/DE/BC.
Keys_Any:
        push hl
        push de
        push bc
        ld a,FN_ANYK
        call WC_API
        pop bc
        pop de
        pop hl
        ret

; Дождаться отпускания клавиш (иначе Enter из меню F10 сразу закрыл бы нас).
Keys_WaitRelease:
        ld a,FN_USPO
        jp WC_API

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
