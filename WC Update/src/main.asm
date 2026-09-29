; ZiFi WC Update для ZX Evolution / TS-Config — проверка и обновление Wild
; Commander Improved с GitHub, аналог sfc /scannow.
;
; Файл задаёт физическую компоновку WMF. Первые 512 байтов — заголовок формата
; #0A, код собирается с логическим адресом #8000. Файловые запросы ESP
; обслуживают те же модули, что у FTP-плагина (vfs.asm, fs.asm из FTP Server),
; поэтому их страница 1 — окно записи 16 КиБ. Таблица списка — страница 2.

        DEVICE ZXSPECTRUM128
        ; OPEN на запись занятое имя не берёт (vfs.asm): остаток прошлой
        ; замены не удаляется, даже если опись его не увидела.
        DEFINE VFS_WRITE_NEW_ONLY
        INCLUDE "wc_api.inc"

startCode:
        ORG #0000
        INCLUDE "wc_header.inc"

        ALIGN 512                         ; код начинается после сектора заголовка
        DISP #8000
mainStart:
        INCLUDE "wcupdate.asm"
        INCLUDE "config.asm"
        INCLUDE "zifi_uart.asm"

; Переходники API Wild Commander. Номер функции передаётся в A и выполняется
; общим входом WC_API. Диспетчер WC меняет AF и AF' местами, поэтому параметр,
; который функция ждёт в A, кладётся в A' через EX AF,AF'.
WC_PRWOW:
        ld a,FN_PRWOW
        jp WC_API
WC_RRESB:
        ld a,FN_RRESB
        jp WC_API
WC_PRSRW:
        ld a,FN_PRSRW
        jp WC_API
WC_PRIAT:
        ex af,af'
        ld a,FN_PRIAT
        jp WC_API
WC_GEDPL:
        ld a,FN_GEDPL
        jp WC_API
WC_TURBOPL:
        ld a,FN_TURBOPL
        jp WC_API
WC_ESC:
        ld a,FN_ESC
        jp WC_API
WC_SPACE:
        ld a,FN_SPACE
        jp WC_API
WC_UP:
        ld a,FN_UP
        jp WC_API
WC_DOWN:
        ld a,FN_DOWN
        jp WC_API
WC_ENTER:
        ld a,FN_ENTER
        jp WC_API
WC_PGUP:
        ld a,FN_PGUP
        jp WC_API
WC_PGDN:
        ld a,FN_PGDN
        jp WC_API
WC_HOME:
        ld a,FN_HOME
        jp WC_API
WC_END:
        ld a,FN_END
        jp WC_API
WC_TXTPR:
        ld a,FN_TXTPR
        jp WC_API
WC_KBSCN:
        ex af,af'
        ld a,FN_KBSCN
        jp WC_API
WC_LOAD512:
        ld a,FN_LOAD512
        jp WC_API
WC_SAVE512:
        ld a,FN_SAVE512
        jp WC_API
WC_STREAM:
        ld a,FN_STREAM
        jp WC_API
WC_FENTRY:
        ld a,FN_FENTRY
        jp WC_API
WC_TENTRY:
        ld a,FN_TENTRY
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

; Таблицы табличного CRC16 для vfs.asm. Выравнивание на 256 обязательно:
; индекс кладётся прямо в L. Заполняет их Vfs_Crc16Init при старте.
        ALIGN 256
VfsCrcHi:       ds 256
VfsCrcLo:       ds 256

mainEnd:
        ; Код и постоянные данные обязаны поместиться в окно #8000..#BFFF.
        ASSERT mainEnd <= #C000, "plugin code exceeds the #8000 page"
        ENT
endCode:
        SAVEBIN "../build/WCUPDATE.WMF",startCode,endCode-startCode
