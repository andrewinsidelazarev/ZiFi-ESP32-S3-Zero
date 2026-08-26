; Автономное обновление ZiFi ESP32-S3 Zero из опубликованной ветки GitHub.

        DEVICE ZXSPECTRUM128
        INCLUDE "wc_api.inc"

startCode:
        ORG #0000
        INCLUDE "wc_header.inc"

        ALIGN 512
        DISP #8000
mainStart:
        INCLUDE "updater.asm"
        DEFINE CONFIG_FULL_INI
        INCLUDE "config.asm"
        INCLUDE "proto.asm"
        INCLUDE "zifi_uart.asm"

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
WC_ADIR:
        ex af,af'
        ld a,FN_ADIR
        jp WC_API
WC_FINDNEXT:
        ex af,af'
        ld a,FN_FINDNEXT
        jp WC_API
WC_INT_PL:
        ex af,af'
        ld a,FN_INT_PL
        jp WC_API

mainEnd:
        ASSERT mainEnd <= #C000, "plugin code exceeds the #8000 page"
        ENT
endCode:
        SAVEBIN "../build/ZIFIUPD.WMF",startCode,endCode-startCode
