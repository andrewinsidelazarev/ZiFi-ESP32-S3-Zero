; Чтение /zifi/zifi.ini через файловый поток FAT из Wild Commander.
; Обязательные ключи сохраняют формат существующих приложений ZiFi:
;   пример имени сети: SSID: ...
;   пример пароля: password: ...
; FTP-параметров в INI нет: сервер всегда использует порт 21 и вход zx/zx.
;
; Wild Commander читает FAT через состояние выбранного STREAM. Поток 0 служит
; текущим каталогом FTP, поток 1 — независимой опорой выбранного тома. Поиск INI
; перебирает допустимые SD-устройства, а после успеха переводит оба потока в их
; корень. Это важно: прямого доступа к портам SD в плагине нет.

CONFIG_SECTOR_SIZE equ 512

; По умолчанию старые плагины сохраняют прежний односекторный предел. Online
; Update задаёт препроцессорный флаг CONFIG_FULL_INI перед INCLUDE.
        IFDEF CONFIG_FULL_INI
CONFIG_MAX_INI_SIZE    equ 1024
CONFIG_INI_BUFFER_SIZE equ 1025
        ELSE
CONFIG_MAX_INI_SIZE    equ CONFIG_SECTOR_SIZE-1
CONFIG_INI_BUFFER_SIZE equ CONFIG_SECTOR_SIZE
        ENDIF

; Найти и прочитать конфигурацию. Старые плагины читают один сектор, а при
; CONFIG_FULL_INI — до двух секторов/1024 байтов. Выход: CF=0 — файл готов;
; CF=1 — ConfigError содержит этап ошибки.
Config_Load:
        call Config_Defaults

        ; STREAM #FE разрешено вызывать только для начального клонирования
        ; активной панели. Повторный вызов вернул бы нас из корня в каталог панели.
        ld d,STREAM_CLONE
        call WC_STREAM

        ; Сначала проверить SD активной панели, затем SD1 и SD2 Z-Controller.
        ; Корень FTP останется на томе с найденным INI.
        call Config_TryActiveSd
        jr nc,.file_open
        ld b,0                         ; SD1 Z-Controller в интерфейсе STREAM
        call Config_TryDevice
        jr nc,.file_open
        ld b,6                         ; SD2 Z-Controller в интерфейсе STREAM
        call Config_TryDevice
        jr nc,.file_open

        ld a,(ConfigHaveRoot)
        or a
        jp z,.stream_error
        ld a,(ConfigDirSeen)
        or a
        jp z,.directory_error
        jp .file_error

.file_open:
        IFDEF CONFIG_FULL_INI
        ; Размер WC_FENTRY приходит как DE:HL. Проверяем его до чтения, чтобы
        ; плагин с полным WIFI_INI не выдавал молча усечённую конфигурацию.
        ld a,d
        or e
        jr nz,.limit_ini
        ld bc,CONFIG_MAX_INI_SIZE+1
        or a
        sbc hl,bc
        jr nc,.limit_ini
        add hl,bc
        ld (IniLength),hl
        jr .length_ready
.limit_ini:
        ld a,4
        jp .fail
        ELSE
        ; Прежний контракт серверных плагинов: взять не более 511 байтов.
        ld a,d
        or e
        jr nz,.limit_ini
        ld a,h
        cp 2
        jr nc,.limit_ini
        ld (IniLength),hl
        jr .length_ready
.limit_ini:
        ld hl,CONFIG_SECTOR_SIZE-1
        ld (IniLength),hl
        ENDIF
.length_ready:
        call WC_GFILE
        ld hl,IniBuffer
        ld b,1
        IFDEF CONFIG_FULL_INI
        ; Ровно 512 байтов помещаются в один сектор; 513..1024 требуют два.
        ld a,(IniLength+1)
        cp 2
        jr c,.load_ini
        jr nz,.load_two
        ld a,(IniLength)
        or a
        jr z,.load_ini
.load_two:
        ld b,2
.load_ini:
        ENDIF
        call WC_LOAD512
        ld de,(IniLength)
        ld hl,IniBuffer
        add hl,de
        xor a
        ld (hl),a

        ; Тело zifi.ini прочитано целиком в IniBuffer; разбор SSID/пароля/времени
        ; выполняет ESP. Плагин файл не интерпретирует — только отдаёт как есть.
        xor a
        ld (ConfigError),a
        call Config_RestoreRoot
        or a
        ret

.stream_error:
        ld a,1
        jr .fail
.directory_error:
        ld a,2
        jr .fail
.file_error:
        ld a,3
        jr .fail
.fail:
        ld (ConfigError),a
        call Config_RestoreRoot
        scf
        ret

; После любой попытки поиска вернуть рабочие потоки в корень найденного тома.
Config_RestoreRoot:
        ld a,(ConfigHaveRoot)
        or a
        ret z
        jp Config_RootBothStreams

; Установить фиксированные параметры FTP и очистить строки Wi-Fi.
Config_Defaults:
        xor a
        ld (ConfigHaveRoot),a
        ld (ConfigDirSeen),a
        ld (ConfigError),a
        ld hl,21
        ld (FtpPort),hl
        ret

; Клонировать активную SD-панель Wild Commander. Номера 1, 2 и 6 обозначают
; соответственно SD1 Z-Controller, SD NeoGS и SD2 Z-Controller.
Config_TryActiveSd:
        ld a,(ConfigPanelDevice)
        cp 1
        jr z,.accepted
        cp 2
        jr z,.accepted
        cp 6
        jr nz,Config_TryFailed
.accepted:
        call Config_RootBothStreams
        jp Config_OpenOnRoot

; B — номер устройства интерфейса STREAM, раздел C в WC не используется.
Config_TryDevice:
        ld a,b
        ld (ConfigTryDevice),a
        ld c,0
        ld d,0
        call WC_STREAM
        jr nz,Config_TryFailed
        ld a,(ConfigTryDevice)
        ld b,a
        ld c,0
        ld d,1
        call WC_STREAM
        jr nz,Config_TryFailed
        call Config_RootBothStreams
        jp Config_OpenOnRoot

; Установить корень отдельно в обоих рабочих потоках. STREAM #FE здесь
; вызывать нельзя: он снова склонировал бы исходный каталог активной панели.
Config_RootBothStreams:
        ld d,0
        ld bc,#FFFF
        call WC_STREAM
        ld d,STREAM_ROOT
        call WC_STREAM
        ld d,1
        ld bc,#FFFF
        call WC_STREAM
        ld d,STREAM_ROOT
        call WC_STREAM
        ld d,0
        ld bc,#FFFF
        call WC_STREAM
        ret

; В текущем корне найти каталог zifi (флаг #10), войти в него и найти обычный
; файл zifi.ini (флаг #00). Успех WC_FENTRY обозначается NZ.
Config_OpenOnRoot:
        ld a,1
        ld (ConfigHaveRoot),a

        ld hl,ConfigDirEntry
        call WC_FENTRY
        jr z,Config_TryFailed
        ld a,1
        ld (ConfigDirSeen),a
        call WC_GDIR
        ld hl,ConfigFileEntry
        call WC_FENTRY
        jr z,Config_TryFailed
        or a
        ret

Config_TryFailed:
        scf
        ret

; DE — ключ вместе с двоеточием. Поиск идёт только с начала непустого участка
; каждой строки, ASCII-регистр букв игнорируется. Разделителями строк считаются
; CR, LF и CRLF — старые ZiFi-файлы встречаются во всех трёх вариантах.
; Выход: CF=1 и HL указывает на значение после горизонтальных пробелов;
; CF=0 — ключ не найден.
CopyZ:
        ld a,(hl)
        ld (de),a
        inc hl
        inc de
        or a
        jr nz,CopyZ
        ret

; Копировать строку HL -> DE без нуля; DE остаётся на первом свободном байте.
CopyZNoTerm:
        ld a,(hl)
        or a
        ret z
        ld (de),a
        inc hl
        inc de
        jr CopyZNoTerm

; FAT-записи и имена INI-ключей.
ConfigDirEntry:       db #10,"zifi",0
ConfigFileEntry:      db #00,"zifi.ini",0
ConfigHaveRoot:       db 0
ConfigDirSeen:        db 0
ConfigPanelDevice:    db 0
ConfigTryDevice:      db 0
ConfigError:          db 0
IniLength:            dw 0
FtpPort:              dw 21
IniBuffer:            ds CONFIG_INI_BUFFER_SIZE
