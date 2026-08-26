; Заголовок .WMF для Wild Commander, формат #0A.
; Плагин запускается пользователем из меню и занимает одну страницу #8000.

        ds 16
        db "WildCommanderMDL"
        db #0A
        db 0
        db 1
        db 0

        db 0,(mainEnd-mainStart+511)/512
        db 0,0
        db 0,0
        db 0,0
        db 0,0
        db 0,0

        ds 16
        ds 32*3
        db 0
        dd 0

.plugin_name
        db "ZiFi Online Update v0.6"
        ds 32-($-.plugin_name),' '
        db #03                        ; ручной запуск из меню плагинов
        ds 6
        ds 32
