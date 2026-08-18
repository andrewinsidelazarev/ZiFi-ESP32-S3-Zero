; SMB-сервер ZiFi для ZX Evolution / TS-Config.
; Плагин меню Wild Commander; ресурс SD совпадает с корнем основной SD-карты.
;
; Файл задаёт физическую компоновку WMF. Первые 512 байтов содержат заголовок
; формата #0A, а исполняемый код собирается с логическим адресом #8000. Wild
; Commander загружает страницу кода в это окно и передаёт управление PLUGIN.
; Page1 принимает файловые блоки и публикуется по 16 КиБ.

        DEVICE ZXSPECTRUM128
        INCLUDE "wc_api.inc"

startCode:
        ORG #0000
        INCLUDE "wc_header.inc"

        ALIGN 512                         ; код начинается после сектора заголовка
        DISP #8000
mainStart:
        ; Порядок INCLUDE важен: smb_server включает UI, сеть и команды SMB,
        ; а config и UART предоставляют используемые ими общие процедуры.
        INCLUDE "smb_server.asm"
        INCLUDE "config.asm"
        INCLUDE "zifi_uart.asm"

; Переходники API Wild Commander. Номер функции передаётся в A и выполняется
; общим входом WC_API. Для функций, где вызывающий код уже использует A как
; параметр, значение временно переносится в альтернативный AF через EX AF,AF'.
WC_PRWOW:
        ld a,FN_PRWOW
        jp WC_API
WC_RRESB:
        ld a,FN_RRESB
        jp WC_API
WC_TXTPR:
        ld a,FN_TXTPR
        jp WC_API
WC_GEDPL:
        ld a,FN_GEDPL
        jp WC_API
WC_ESC:
        ld a,FN_ESC
        jp WC_API
WC_LOAD512:
        ld a,FN_LOAD512
        jp WC_API
WC_SAVE512:
        ld a,FN_SAVE512
        jp WC_API
WC_TENTRY:
        ld a,FN_TENTRY
        jp WC_API
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
WC_MKFILE:
        ld a,FN_MKFILE
        jp WC_API
WC_MKDIR:
        ld a,FN_MKDIR
        jp WC_API
WC_RENAME:
        ld a,FN_RENAME
        jp WC_API
WC_DELETE:
        ld a,FN_DELETE
        jp WC_API
WC_APPEND:
        ld a,FN_APPEND
        jp WC_API
WC_FILEX:
        ld a,FN_FILEX
        jp WC_API
WC_ADIR:
        ex af,af'
        ld a,FN_ADIR
        jp WC_API
WC_FINDNEXT:
        ex af,af'
        ld a,FN_FINDNEXT
        jp WC_API
WC_MNGC_PL:
        ex af,af'
        ld a,FN_MNGC_PL
        jp WC_API
WC_MNG8_PL:
        ex af,af'
        ld a,FN_MNG8_PL
        jp WC_API
WC_INT_PL:
        ex af,af'
        ld a,FN_INT_PL
        jp WC_API

mainEnd:
        ; Код и постоянные данные обязаны поместиться в окно #8000..#BFFF.
        ASSERT mainEnd <= #C000, "plugin code exceeds the #8000 page"
        ENT
endCode:
        SAVEBIN "../build/ZIFISMB.WMF",startCode,endCode-startCode
