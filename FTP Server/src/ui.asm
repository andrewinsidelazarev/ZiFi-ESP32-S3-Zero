; Текстовое окно состояния для Wild Commander.
;
; UiText — готовый шаблон с полями фиксированной ширины. Обновление состояния
; меняет байты прямо в шаблоне, после чего WC_TXTPR перерисовывает всё окно.
; Такой подход не требует хранить координаты каждой строки и предсказуемо работает
; с кодировкой экрана Wild Commander.

UI_FIELD_STATUS equ 30
UI_FIELD_IP     equ 32
UI_FIELD_FW     equ 30
UI_FIELD_CLIENT equ 30
UI_FIELD_LAST   equ 30
UI_FIELD_PORT   equ 5

; Создать окно, сохранить закрываемую им область экрана и заполнить динамические
; поля начальными значениями.
Ui_Open:
        ld ix,FtpWindow
        call WC_PRWOW
        ld hl,UiClientNone
        call Ui_SetClient
        ld hl,UiLastNone
        call Ui_SetLast
        ld hl,UiFirmwareUnknown
        call Ui_SetFirmware
        call Ui_BuildPort
        ret

; Перерисовать содержимое окна с позиции (1,1) относительно его клиентской части.
Ui_Draw:
        ld ix,FtpWindow
        ld hl,UiText
        ld de,#0101
        jp WC_TXTPR

; HL — нуль-терминированная строка нового состояния.
Ui_SetStatus:
        ld de,UiStatusField
        ld b,UI_FIELD_STATUS
        jp Ui_CopyField

; HL — строка версии нативной прошивки либо явная пометка об отсутствии SYS_INFO.
Ui_SetFirmware:
        ld de,UiFirmwareField
        ld b,UI_FIELD_FW
        jp Ui_CopyField

; HL — описание управляющего FTP-клиента.
Ui_SetClient:
        ld de,UiClientField
        ld b,UI_FIELD_CLIENT
        jp Ui_CopyField

; HL — последняя принятая FTP-команда; управляющие байты заменяются точками.
Ui_SetLast:
        ld de,UiLastField
        ld b,UI_FIELD_LAST
        jp Ui_CopyField

; HL — исходная строка с нулём, DE — поле, B — ширина поля.
Ui_CopyField:
        push hl
        push de
        push bc
        ld a,' '
.clear:
        ld (de),a
        inc de
        djnz .clear
        pop bc
        pop de
        pop hl
.copy:
        ld a,b
        or a
        ret z
        ld a,(hl)
        or a
        ret z
        cp 32
        jr nc,.printable
        ld a,'.'
.printable:
        ld (de),a
        inc de
        inc hl
        dec b
        jr .copy

; Перевести порт из тела FTP_START в десятичный текст без ведущих нулей.
; Поле использует тот же LinkFtpPort, который реально отправляется ESP.
Ui_BuildPort:
        ld de,UiPortField
        ld b,UI_FIELD_PORT
        ld a,' '
.clear:
        ld (de),a
        inc de
        djnz .clear
        ld de,UiPortField
        ld hl,(LinkFtpPort)
        jp U16_ToDec

; Собрать отображаемый IPv4 без схемы и порта. Поле предварительно очищается
; пробелами, чтобы короткий новый адрес не оставил хвост старого.
;
; ВАЖНО: LocalIp — это ЧЕТЫРЕ ДВОИЧНЫХ БАЙТА от ESP, а не строка. Прежний код
; копировал их через CopyZNoTerm, то есть до первого нулевого байта: он вылетал
; за пределы четырёх байт, гнал мусор в поле, переполнял его и затирал чужую
; память (мусор на экране и следы после выхода из плагина). Теперь каждый октет
; переводится в десятичный текст явно.
Ui_BuildIp:
        ld de,UiIpField
        ld b,UI_FIELD_IP
        ld a,' '
.clear:
        ld (de),a
        inc de
        djnz .clear
        ld de,UiIpField
        ld hl,LocalIp
        ld b,4                         ; четыре октета через точку
.octet:
        push bc
        push hl
        ld l,(hl)
        ld h,0
        call U16_ToDec                 ; DE сдвигается за записанные цифры
        pop hl
        pop bc
        inc hl
        dec b
        ret z
        ld a,'.'
        ld (de),a
        inc de
        jr .octet

; Дескриптор окна формата Wild Commander. Текст не привязан к дескриптору:
; содержимое печатается отдельно через TXTPR, а RRESB восстанавливает фон.
FtpWindow:
        db #81                         ; тень и рамка стиля 1
        db 0
        db 13,5                        ; координаты X,Y
        db 52,19                       ; ширина и высота
        db #17                         ; синий фон, ярко-белые символы
        db 0
        dw 0
        db 0,0
        dw UiHeader
        dw 0
        dw 0                           ; содержимое выводится через TXTPR

UiHeader:
        db #0E,9," ZiFi FTP Server ",0

UiText:
        db #0E,"FTP server for ZX Evolution / TS-Config",#0D,#0D
        db "Status : "
UiStatusField:
        ds UI_FIELD_STATUS,' '
        db #0D
        db "IP     : "
UiIpField:
        ds UI_FIELD_IP,' '
        db #0D
        db "Port   : "
UiPortField:
        ds UI_FIELD_PORT,' '
        db #0D
        db "Firmware: "
UiFirmwareField:
        ds UI_FIELD_FW,' '
        db #0D,#0D
        db "Root   : /  (whole selected SD card)",#0D
        db "Access : read / write",#0D
        db "Data   : passive FTP (active fallback)",#0D,#0D
        db "Client : "
UiClientField:
        ds UI_FIELD_CLIENT,' '
        db #0D
        db "Last   : "
UiLastField:
        ds UI_FIELD_LAST,' '
        db #0D,#0D
        db #0E,"ESC - stop server and return to WC",0

UiStageConfig:     db "Reading /zifi/zifi.ini",0
UiStageZifi:       db "Detecting ZiFi",0
UiStageWifiSync:   db "Waiting for ESP (READY)",0
UiStageWifiJoin:   db "Connecting to Wi-Fi",0
UiStageDhcp:       db "Waiting for IP address",0
UiStageFtp:        db "Starting FTP listener",0
UiWifiWaiting:     db "Wi-Fi [................]   0%",0
UiStageStopping:   db "Stopping",0
UiErrorSd:         db "ERROR: no readable SD volume",0
UiErrorDir:        db "ERROR: /zifi directory missing",0
UiErrorIni:        db "ERROR: /zifi/zifi.ini missing",0
UiErrorConfig:     db "ERROR: invalid zifi.ini",0
UiErrorSsid:       db "ERROR: SSID missing or empty",0
UiErrorPassword:   db "ERROR: password key missing",0
UiErrorNoZifi:     db "ERROR: ZiFi not detected",0
UiErrorWifi:       db "ERROR: Wi-Fi connection failed",0
UiErrorIp:         db "ERROR: no DHCP IP address",0
UiErrorServer:     db "ERROR: FTP listener failed",0
UiErrorFirmware:   db "ERROR: ESP does not answer (no READY)",0
UiClientNone:      db "not connected",0
UiClientConnected: db "connected, login required",0
UiClientLogged:    db "connected and logged in",0
UiLastNone:        db "-",0
UiFirmwareUnknown: db "detecting...",0
UiFirmwareSilent:  db "ESP silent: no SYS_INFO",0
