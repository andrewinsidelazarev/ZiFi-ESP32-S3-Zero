; Небольшое окно автономного обновлятора. Текст остаётся английским, потому что
; экран Wild Commander использует собственную однобайтовую кодировку.

UI_STATUS_WIDTH  equ 38
UI_VERSION_WIDTH equ 30
UI_PROGRESS_BAR_WIDTH equ 32

Ui_Open:
        ld ix,UpdateWindow
        call WC_PRWOW
        ld hl,UiUnknown
        call Ui_SetVersion
        ld hl,UiUnknown
        call Ui_SetAvailable
        ld hl,UiReadingConfig
        call Ui_SetStatus
        xor a
        call Ui_SetProgress
        ret

Ui_Draw:
        ld ix,UpdateWindow
        ld hl,UiText
        ld de,#0101
        jp WC_TXTPR

Ui_SetStatus:
        ld de,UiStatusField
        ld b,UI_STATUS_WIDTH
        jp Ui_CopyField

Ui_SetVersion:
        ld de,UiVersionField
        ld b,UI_VERSION_WIDTH
        jp Ui_CopyField

Ui_SetAvailable:
        ld de,UiAvailableField
        ld b,UI_VERSION_WIDTH
        jp Ui_CopyField

; A=0..100. Полоса содержит 32 знакоместа, справа остаётся точное значение.
Ui_SetProgress:
        ld (UiPercentValue),a

        ; filled = floor(percent * 32 / 100). Произведение помещается в HL:
        ; максимум 3200, поэтому достаточно обычного повторного вычитания.
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        ld de,100
        ld b,0
.divide:
        or a
        sbc hl,de
        jr c,.draw_bar
        inc b
        jr .divide
.draw_bar:
        ld hl,UiProgressBarField
        ld c,UI_PROGRESS_BAR_WIDTH
.bar_cell:
        ld a,b
        or a
        jr z,.empty_cell
        ld a,'#'
        ld (hl),a
        dec b
        jr .next_cell
.empty_cell:
        ld a,'.'
        ld (hl),a
.next_cell:
        inc hl
        dec c
        jr nz,.bar_cell

        ld hl,UiProgressField
        ld (hl),' '
        inc hl
        ld (hl),' '
        inc hl
        ld (hl),' '
        ld a,(UiPercentValue)
        ld e,a
        ld d,0
        ld b,'0'
.hundreds:
        ld a,e
        cp 100
        jr c,.hundreds_done
        sub 100
        ld e,a
        inc b
        jr .hundreds
.hundreds_done:
        ld a,b
        cp '0'
        jr z,.tens_start
        ld (UiProgressField),a
.tens_start:
        ld b,'0'
.tens:
        ld a,e
        cp 10
        jr c,.tens_done
        sub 10
        ld e,a
        inc b
        jr .tens
.tens_done:
        ld a,b
        cp '0'
        jr nz,.write_tens
        ld a,(UiProgressField)
        cp ' '
        jr z,.ones
        ld a,'0'
.write_tens:
        ld (UiProgressField+1),a
.ones:
        ld a,e
        add a,'0'
        ld (UiProgressField+2),a
        ret

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

UpdateWindow:
        db #81
        db 0
        db 10,6
        db 60,18
        db #17
        db 0
        dw 0
        db 0,0
        dw UiHeader
        dw 0
        dw 0

UiHeader:
        db #0E,9," ZiFi firmware update ",0

UiText:
        db #0E,"ESP32-S3 online updater",#0D,#0D
        db "Source : GitHub main / firmware.bin",#0D
        db "Current: "
UiVersionField:
        ds UI_VERSION_WIDTH,' '
        db #0D
        db "Available: "
UiAvailableField:
        ds UI_VERSION_WIDTH,' '
        db #0D
        db "Status : "
UiStatusField:
        ds UI_STATUS_WIDTH,' '
        db #0D
        db "Progress: ["
UiProgressBarField:
        ds UI_PROGRESS_BAR_WIDTH,'.'
        db "] "
UiProgressField:
        ds 3,' '
        db "%",#0D,#0D
        db "ENTER - install newer / reinstall same version",#0D
        db "ESC   - cancel / return to Wild Commander",#0D,#0D
        db #0E,"Do not switch power off after confirmation.",0

UiUnknown:          db "detecting...",0
UiUnavailable:      db "unavailable",0
UiReadingConfig:    db "Reading /zifi/zifi.ini",0
UiDetecting:        db "Detecting ZiFi firmware",0
UiResettingZiFi:    db "Restarting ZiFi before update",0
UiConnecting:       db "Connecting to Wi-Fi",0
UiChecking:         db "Checking published firmware",0
UiSameVersion:      db "Same version. ENTER = reinstall",0
UiNewerVersion:     db "New version available. ENTER = install",0
UiOlderVersion:     db "Older version. DOWNGRADE BLOCKED",0
UiDifferentVersion: db "Different build. ENTER = install",0
UiManifest:         db "Reading version and SHA-256",0
UiDownloading:      db "Downloading firmware.bin",0
UiVerifying:        db "Checking SHA-256 and installing",0
UiRestarting:       db "SHA-256 OK. ESP is restarting",0
UiCompleted:        db "Update completed. ESC = return",0
UiRestartUnknown:   db "Installed; ESP restart not confirmed",0
UiCancelled:        db "Cancelled",0
UiErrorSd:          db "ERROR: no readable SD volume",0
UiErrorDir:         db "ERROR: /zifi directory missing",0
UiErrorIni:         db "ERROR: /zifi/zifi.ini missing",0
UiErrorIniLarge:    db "ERROR: zifi.ini exceeds 1024 bytes",0
UiErrorNoZifi:      db "ERROR: ZiFi not detected",0
UiErrorFirmware:    db "ERROR: incompatible ESP firmware",0
UiErrorWifi:        db "ERROR: Wi-Fi connection failed",0
UiErrorUpdate:      db "ERROR: online update failed",0
UiErrorSilent:      db "ERROR: ESP stopped responding",0

UiPercentValue:     db 0
