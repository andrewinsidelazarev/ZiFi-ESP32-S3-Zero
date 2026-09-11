; Переходники API Wild Commander для заставок погоды (TS-Conf и VDAC2).
;
; Общий вход WC_API (#6006) получает номер функции в A. Там, где A уже занят
; параметром, значение передаётся через альтернативный AF (EX AF,AF').

; Функции WC, которых нет в общем wc_api.inc.
FN_ANYK         equ 45                  ; нажата ли любая клавиша (NZ — да)
FN_USPO         equ 46                  ; ждать отпускания всех клавиш
FN_MNGV_PL      equ 64                  ; выбрать банк видеостраниц (0 — текст WC)
FN_MNGCVPL      equ 65                  ; видеостраница A' -> окно #C000
FN_GVMOD        equ 66                  ; задать видеорежим плагина
FN_MNG0_PL      equ 78                  ; страница плагина A' -> окно #0000

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
