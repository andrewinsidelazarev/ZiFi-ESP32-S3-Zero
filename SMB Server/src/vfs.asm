; Файловые запросы ESP: плагин отвечает на них через DOS API Wild Commander.
;
; SMB-протокол разбирает ESP, поэтому Z80 получает только файловые приказы:
; только файловые приказы, ответ уходит с тем же кодом команды:
;
;   #40 STAT     путь,0            -> [статус][каталог][размер:4]
;   #41 OPENDIR  путь,0            -> [статус]
;   #42 READDIR  —                 -> [статус][каталог][размер:4][имя…]
;   #43 FSINFO   [refresh]          -> [статус][FILEX FS info:48]
;   #44 FATWIN   [seq][offset:4][length:2] -> карта занятости активной FAT
;   #50 OPEN     [режим]путь,0     -> [статус][caps][filex caps]
;                                      0=чтение, 1=замена, 2=append, 3=FILEX
;   #51 READ     [сколько:2]       -> [статус][данные…]
;   #52 WRITE    устаревший raw     -> [отказ]
;   #53 CLOSE    —                 -> [статус]
;   #54 DELETE   путь,0            -> [статус]
;   #55 MKDIR    путь,0            -> [статус]
;   #56 BLOCK    LZ4/RAW-фрагмент   -> [статус][seq][принято:2]
;   #57 WRITEWIN raw-окно 16 КиБ    -> один [статус][seq][принято:2]
;   #58 READWIN  [seq][сколько:2]   -> поток кадров одного окна
;   #59 RENAME   [attr]старый,0,новый,0 -> [статус]
;   #5A EXTEND   [сколько:4]        -> [статус], дописать нули через APPEND
;   #5B SEEK     [смещение:4]       -> [статус]
;   #5C SETEOF   [размер:4]         -> [статус][фактический размер:4]
;   #5D MOVE     [флаги][атрибут]старый,0,новый,0 -> [статус FILEX]
;   #5E SETMETA  [метаданные FILEX:16] -> [статус][применённый атрибут]
;
; Статус 0 — успех, иначе отказ. Для READDIR ненулевой статус означает конец
; каталога, для READ — конец файла.
;
; Режимы 1/2 сохраняют старый последовательный тракт MKFILE/APPEND для FTP и
; совместимости. Режим 3 использует FILEX READ_AT/WRITE_AT с 32-битным
; смещением; он поддерживает произвольные чтение, запись и SET_EOF для SMB.

VFS_STAT        equ #40
VFS_OPENDIR     equ #41
VFS_READDIR     equ #42
VFS_FSINFO      equ #43
VFS_FAT_WINDOW  equ #44
VFS_OPEN        equ #50
VFS_READ        equ #51
VFS_WRITE       equ #52
VFS_CLOSE       equ #53
VFS_DELETE      equ #54
VFS_MKDIR       equ #55
VFS_BLOCK       equ #56
VFS_WRITE_WINDOW equ #57
VFS_READ_WINDOW equ #58
VFS_RENAME      equ #59
VFS_EXTEND      equ #5A
VFS_SEEK        equ #5B
VFS_SET_EOF     equ #5C
VFS_MOVE_RENAME equ #5D
VFS_SET_METADATA equ #5E

VFS_OK          equ 0
VFS_FAIL        equ 1
VFS_CAP_WINDOWS equ %00000111          ; биты 0/1/2: окна записи/чтения, EXTEND
VFS_WIRE_SIZE   equ 256                 ; один подтверждаемый пакет ESP -> Z80
VFS_BLOCK_SIZE  equ #0200               ; независимый исходный блок: до 512 байт
VFS_APPEND_SIZE equ #4000               ; один физический commit APPEND: до 16 КиБ
VFS_FILEX_DATA  equ #8020
VFS_FILEX_DATA_C equ #C020
VFS_FILEX_WINDOW equ #3FE0
VFS_BLOCK_HEAD  equ 8
VFS_CONT_HEAD   equ 4
VFS_WINDOW_HEAD equ 8
VFS_READ_HEAD   equ 9
VFS_WINDOW_START equ 1
VFS_WINDOW_END  equ 2
VFS_WINDOW_DATA equ PROTO_MAX-VFS_WINDOW_HEAD
VFS_READ_DATA   equ PROTO_MAX-VFS_READ_HEAD
VFS_FAT_DATA    equ (VFS_READ_DATA/4)*4

VFS_BS_STATE    equ 1
VFS_BS_SEQ      equ 2
VFS_BS_LENGTH   equ 3
VFS_BS_OFFSET   equ 4
VFS_BS_LZ4      equ 5
VFS_BS_CRC      equ 6
VFS_BS_OVERFLOW equ 7
VFS_BS_APPEND   equ 8

; Разобрать пакет: A=команда, payload в ProtoBuf, длина в ProtoRxLen.
; Выход: CF=1 — это был файловый запрос и на него уже отвечено.
Vfs_Dispatch:
        cp VFS_STAT
        jp z,Vfs_Stat
        cp VFS_OPENDIR
        jp z,Vfs_OpenDir
        cp VFS_READDIR
        jp z,Vfs_ReadDir
        cp VFS_FSINFO
        jp z,Vfs_FsInfo
        cp VFS_FAT_WINDOW
        jp z,Vfs_FatWindow
        cp VFS_OPEN
        jp z,Vfs_Open
        cp VFS_READ
        jp z,Vfs_Read
        cp VFS_WRITE
        jp z,Vfs_Write
        cp VFS_CLOSE
        jp z,Vfs_Close
        cp VFS_DELETE
        jp z,Vfs_Delete
        cp VFS_MKDIR
        jp z,Vfs_Mkdir
        cp VFS_BLOCK
        jp z,Vfs_Block
        cp VFS_WRITE_WINDOW
        jp z,Vfs_WriteWindow
        cp VFS_READ_WINDOW
        jp z,Vfs_ReadWindow
        cp VFS_RENAME
        jp z,Vfs_Rename
        cp VFS_EXTEND
        jp z,Vfs_Extend
        cp VFS_SEEK
        jp z,Vfs_Seek
        cp VFS_SET_EOF
        jp z,Vfs_SetEof
        cp VFS_MOVE_RENAME
        jp z,Vfs_MoveRename
        cp VFS_SET_METADATA
        jp z,Vfs_SetMetadata
        or a                            ; не наш пакет
        ret

; --- ответы ------------------------------------------------------------------

; Отправить короткий ответ: A=команда, C=статус.
Vfs_Reply1:
        ld (VfsCmd),a
        ld a,c
        ld (VfsBuf),a
        ld a,(VfsCmd)
        ld hl,VfsBuf
        ld bc,1
        call Proto_Send
        scf
        ret

; Успешный OPEN возвращает возможности окон и фактическую маску FILEX.
; Старые прошивки проверяют только status[0] и безопасно игнорируют хвост.
Vfs_OpenReply:
        xor a
        ld (VfsBuf),a
        ld a,VFS_CAP_WINDOWS
        ld (VfsBuf+1),a
        ld a,(VfsFilexCaps)
        ld (VfsBuf+2),a
        ld a,VFS_OPEN
        ld hl,VfsBuf
        ld bc,3
        call Proto_Send
        scf
        ret

; Отказ на текущую команду.
Vfs_Refuse:
        ld a,(VfsCmd)
        ld c,VFS_FAIL
        jr Vfs_Reply1

; Ответ на подтверждаемый фрагмент BLOCK:
; [статус][seq][число принятых stored-байтов LE16].
; Вход: A=статус, seq уже сохранён в VfsReplySeq.
Vfs_BlockReply:
        ld (VfsBuf),a
        ld a,(VfsReplySeq)
        ld (VfsBuf+1),a
        ld hl,(VfsBlockReceived)
        ld (VfsBuf+2),hl
        ld a,VFS_BLOCK
        ld hl,VfsBuf
        ld bc,4
        call Proto_Send
        scf
        ret

; После ошибки текущую запись запрещено продолжать вслепую.
; Вход: A=VFS_BS_*.
Vfs_BlockFail:
        push af
        ld a,1
        ld (VfsWriteFailed),a
        xor a
        ld (VfsBlockActive),a
        pop af
        jp Vfs_BlockReply

; --- разбор пути -------------------------------------------------------------

; Скопировать путь из ProtoBuf (со смещением HL) в VfsPath, перевести имя из
; UTF-8 в кодировку WC. Выход: CF=1 при слишком длинном пути.
Vfs_TakePath:
        ld de,VfsPath
        ld b,PATH_SIZE-1
.copy:
        ld a,(hl)
        ld (de),a
        or a
        jr z,.done
        inc hl
        inc de
        djnz .copy
        scf
        ret
.done:
        ld hl,VfsPath
        call Utf8_ToOemInPlace          ; дальше работаем в кодировке имён WC
        or a
        ret

; Разделить VfsPath на каталог и последний компонент.
; Каталог применяется к текущему потоку, имя остаётся в VfsName.
; Выход: CF=1 — путь не разобран; Z=1 — путь указывает на корень (имени нет).
Vfs_SplitPath:
        ; Найти последнюю косую черту.
        ld hl,VfsPath
        ld de,0                         ; DE = адрес последней '/'
.scan:
        ld a,(hl)
        or a
        jr z,.scanned
        cp '/'
        jr nz,.next
        ld d,h
        ld e,l
.next:
        inc hl
        jr .scan
.scanned:
        ld a,d
        or e
        jr nz,.have_slash
        ; Косой черты нет вообще — имя в текущем каталоге.
        ld hl,VfsPath
        ld de,VfsName
        call Vfs_CopyZ
        or a
        ld a,1                          ; NZ: имя есть
        or a
        ret

.have_slash:
        ; Имя — всё после последней косой черты.
        ld h,d
        ld l,e
        inc hl
        push hl
        ld de,VfsName
        call Vfs_CopyZ
        pop hl
        ; Каталог — часть до косой черты; обрезаем её нулём (корень остаётся '/').
        ld a,(VfsName)
        ld (VfsHasName),a
        ld hl,VfsPath
        ld de,0
.scan2:
        ld a,(hl)
        or a
        jr z,.cut
        cp '/'
        jr nz,.next2
        ld d,h
        ld e,l
.next2:
        inc hl
        jr .scan2
.cut:
        ld h,d
        ld l,e
        ld a,h
        or l
        jr z,.root_only
        ; Если косая черта первая — каталог это корень.
        ld de,VfsPath
        push hl
        or a
        sbc hl,de
        pop hl
        jr nz,.trim
        inc hl                          ; оставить "/" как есть
        ld (hl),0
        dec hl
        jr .apply
.trim:
        ld (hl),0
.apply:
        ld hl,VfsPath
        call Fs_ChangePath
        ret c
        ld a,(VfsHasName)
        or a
        ret
.root_only:
        xor a                           ; Z: имени нет, это корень
        ret

; Скопировать строку с нулём: HL -> DE.
Vfs_CopyZ:
        ld a,(hl)
        ld (de),a
        or a
        ret z
        inc hl
        inc de
        jr Vfs_CopyZ

; Перейти в корень потока и разобрать путь. A=атрибут для FsEntry.
; Выход: CF=1 — не разобрано, Z=1 — это корень.
Vfs_Locate:
        ld (VfsAttr),a
        ld hl,ProtoBuf
        call Vfs_TakePath
        ret c
        call Fs_ResetRoot
        call Vfs_SplitPath
        ret c
        ret z                           ; корень
        ; Собрать FsEntry: [атрибут][имя,0]
        ld a,(VfsAttr)
        ld (FsEntry),a
        ld hl,VfsName
        ld de,FsEntry+1
        call Vfs_CopyZ
        ld a,1
        or a                            ; NZ: имя есть
        ret

; --- STAT: сведения о файле или каталоге -------------------------------------

Vfs_Stat:
        ld a,VFS_STAT
        ld (VfsCmd),a
        xor a                           ; ищем сначала как файл
        call Vfs_Locate
        jp c,Vfs_Refuse
        jr z,.root
        call Fs_SelectWork
        ld hl,FsEntry
        call WC_FENTRY
        jr nz,.file_found
        ; Не файл — попробуем как каталог.
        ld a,#10
        ld (FsEntry),a
        call Fs_SelectWork
        ld hl,FsEntry
        call WC_FENTRY
        jp z,Vfs_Refuse
        ld hl,0
        ld de,0
        ld a,1
        jr .answer
.file_found:
        ; FENTRY вернул размер в HL:DE.
        xor a
.answer:
        ld (VfsBuf+1),a                 ; признак каталога
        ld (VfsBuf+2),hl
        ld (VfsBuf+4),de
        xor a
        ld (VfsBuf),a                   ; статус: успех
        jr .send
.root:
        xor a
        ld (VfsBuf),a
        ld a,1
        ld (VfsBuf+1),a                 ; корень — каталог
        ld hl,0
        ld (VfsBuf+2),hl
        ld (VfsBuf+4),hl
.send:
        ld a,VFS_STAT
        ld hl,VfsBuf
        ld bc,6
        call Proto_Send
        scf
        ret

; --- OPENDIR и READDIR: перечисление каталога --------------------------------

Vfs_OpenDir:
        ld a,VFS_OPENDIR
        ld (VfsCmd),a
        ld hl,ProtoBuf
        call Vfs_TakePath
        jp c,Vfs_Refuse
        call Fs_ResetRoot
        ld hl,VfsPath
        ld a,(hl)
        cp '/'
        jr nz,.walk
        inc hl
        ld a,(hl)
        or a
        jr z,.ready                     ; сам корень — уже там
        dec hl
.walk:
        call Fs_ChangePath
        jp c,Vfs_Refuse
.ready:
        ; Начать перечисление каталога. A=1 — как в проверенном коде WC.
        call Fs_SelectWork
        ld a,1
        call WC_ADIR
        ld a,VFS_OPENDIR
        ld c,VFS_OK
        jp Vfs_Reply1

Vfs_ReadDir:
        ld a,VFS_READDIR
        ld (VfsCmd),a
        call Fs_SelectWork
        ; FINDNEXT принимает буфер в DE, а в A — набор запрошенных полей.
        ; #1C = размер, дата и время, любая запись. Раскладка ответа WC:
        ; 0..3 размер, 4..7 дата и время, 8 атрибут, 9 имя с нулём.
        ld de,DirEntryBuffer
        ld a,#1C
        call WC_FINDNEXT
        jp z,Vfs_Refuse                 ; каталог кончился

        xor a
        ld (VfsBuf),a                   ; статус: успех
        ld a,(DirEntryBuffer+8)         ; атрибут: бит 4 — каталог
        and #10
        jr z,.file
        ld a,1
.file:
        ld (VfsBuf+1),a
        ld hl,DirEntryBuffer            ; размер отдаём как есть, 4 байта
        ld de,VfsBuf+2
        ld bc,4
        ldir
        ; Имя из кодировки WC в UTF-8 прямо в тело ответа.
        ld hl,DirEntryBuffer+9
        ld de,VfsBuf+6
        call Oem_CopyUtf8NoTerm
        ; Длина ответа = 6 служебных байт плюс длина имени.
        ld hl,VfsBuf+6
        or a
        ex de,hl
        sbc hl,de
        ld bc,6
        add hl,bc
        ld b,h
        ld c,l
        ld a,VFS_READDIR
        ld hl,VfsBuf
        call Proto_Send
        scf
        ret

; --- OPEN, READ, WRITE, CLOSE: работа с одним файлом -------------------------

Vfs_Open:
        ld a,VFS_OPEN
        ld (VfsCmd),a
        ld a,(ProtoBuf)                 ; 0=чтение, 1=замена, 2=дозапись, 3=FILEX
        ld (VfsMode),a
        xor a
        ld (VfsFilexCaps),a
        ; Путь идёт со второго байта.
        ld hl,ProtoBuf+1
        call Vfs_TakePath
        jp c,Vfs_Refuse
        call Fs_ResetRoot
        call Vfs_SplitPath
        jp c,Vfs_Refuse
        jp z,Vfs_Refuse                 ; корень открыть как файл нельзя
        xor a
        ld (FsEntry),a
        ld hl,VfsName
        ld de,FsEntry+1
        call Vfs_CopyZ

        ; Новый OPEN всегда начинает новый файловый контекст. Для записи это
        ; также сбрасывает ещё не опубликованный остаток предыдущей записи.
        call Vfs_WriteReset
        ld a,(VfsMode)
        cp 2
        jr z,.for_append
        cp 3
        jr z,.for_random
        or a
        jr z,.for_read
        cp 1
        jr z,.for_write
        jp Vfs_Refuse                  ; неизвестный режим нельзя считать записью

        ; Чтение: найти файл и открыть его.
.for_read:
        call Fs_SelectWork
        ld hl,FsEntry
        call WC_FENTRY
        jp z,Vfs_Refuse
        ld (FileSize),hl
        ld (FileSize+2),de
        call WC_GFILE
        ld hl,FileSize
        ld de,FileRemaining
        ld bc,4
        ldir
        jp Vfs_OpenReply

.for_random:
        ; FILEX использует контекст FENTRY, но не продвигает поток GFILE.
        call Fs_SelectWork
        ld hl,FsEntry
        call WC_FENTRY
        jr nz,.random_found
        ld a,#10                       ; метаданные допустимы и для каталогов
        ld (FsEntry),a
        call Fs_SelectWork
        ld hl,FsEntry
        call WC_FENTRY
        jp z,Vfs_Refuse
.random_found:
        ld (FileSize),hl
        ld (FileSize+2),de
        ; Успешный публичный FENTRY уже фиксирует FILEX-контекст вместе с
        ; точным сектором/смещением SFN. Повторный TENTRY здесь недопустим:
        ; после возврата из FENTRY альтернативные регистры не являются частью
        ; публичного ABI, а TENTRY использует их для захвата положения записи.
        call Vfs_FilexQueryCaps
        jp nz,Vfs_Refuse
        ld hl,0
        ld (VfsRandomOffset),hl
        ld (VfsRandomOffset+2),hl
        jp Vfs_OpenReply

.for_append:
        ; SMB при последовательной записи открывает уже существующий файл ещё
        ; раз и продолжает APPEND с его физического конца. Старое содержимое
        ; здесь не удаляется. Данные всё равно идут теми же окнами по 16 КиБ.
        call Fs_SelectWork
        ld hl,FsEntry
        call WC_FENTRY
        jp z,Vfs_Refuse
        call Vfs_MapData
        jp Vfs_OpenReply

.for_write:
        ; Запись: старый файл удалить, создать пустой, зафиксировать FENTRY.
        call Fs_SelectWork
        ld hl,FsEntry
        call WC_FENTRY
        jr z,.create
        ld hl,FsEntry
        call WC_DELETE
        jp z,Vfs_Refuse
.create:
        call Vfs_BuildMkFile
        ld hl,MkFileBuffer
        call WC_MKFILE
        jp nz,Vfs_Refuse
        call Fs_SelectWork
        ld hl,FsEntry
        call WC_FENTRY
        jp z,Vfs_Refuse
        call Vfs_MapData
        jp Vfs_OpenReply

; Собрать точный ABI WildCommander Improved для API 72 MKFILE:
; [атрибут][размер LE32][имя,0]. Пустой файл с размером 0 поддерживается
; исправленным CORE32 и затем потоково расширяется через API 76 APPEND.
Vfs_BuildMkFile:
        ld de,MkFileBuffer
        xor a
        ld (de),a                       ; обычный файл: атрибут 0
        inc de
        ld b,4
.zero_size:
        ld (de),a                       ; размер LE32 = 0
        inc de
        djnz .zero_size
        ld hl,VfsName
        jp Vfs_CopyZ

; Подготовить статический 32-байтовый блок FILEX в code-page.
Vfs_FilexReset:
        ld hl,VfsFilexBlock
        ld de,VfsFilexBlock+1
        ld bc,FILEX_BLOCK_SIZE-1
        xor a
        ld (hl),a
        ldir
        ld a,FILEX_BLOCK_SIZE
        ld (VfsFilexBlock),a
        ld a,FILEX_API_VERSION
        ld (VfsFilexBlock+1),a
        ret

Vfs_FilexQueryCaps:
        call Vfs_FilexReset
        ld a,FILEX_OP_QUERY_CAPS
        ld (VfsFilexBlock+FILEX_P_OPERATION),a
        ld hl,VfsFilexBlock
        call WC_FILEX
        ret nz
        ld a,(VfsFilexBlock+FILEX_P_RESULT_FLAGS)
        cp FILEX_API_VERSION
        ret nz
        ld a,(VfsFilexBlock+FILEX_P_RESULT_COUNT)
        ld (VfsFilexCaps),a
        ; QUERY_CAPS сообщает возможности ненулевой маской. Для вызывающего
        ; OPEN=3 успех, наоборот, обязан возвращаться с Z; иначе исправный
        ; провайдер #3F ошибочно попадал в Vfs_Refuse ещё до READ_AT.
        xor a
        ret

Vfs_Read:
        ld a,VFS_READ
        ld (VfsCmd),a
        ; Файл кончился — статус отказа, ESP поймёт как конец.
        ld hl,FileRemaining
        call U32_IsZero
        jp z,Vfs_Refuse
        ; Сектор читаем прямо в тело ответа, лишних копирований нет.
        ld hl,VfsBuf+1
        ld b,1
        call WC_LOAD512
        ; Отдаём min(остаток, 512).
        ld hl,FileRemaining
        ld a,(hl)
        ld c,a
        inc hl
        ld a,(hl)
        ld b,a
        inc hl
        ld a,(hl)
        inc hl
        or (hl)
        jr nz,.full                     ; старшие байты не нули — точно >512
        ld a,b
        cp 2
        jr nc,.full
        or c
        jr z,.full
        ; BC уже равен остатку (меньше 512).
        jr .subtract
.full:
        ld bc,IO_SIZE
.subtract:
        push bc
        call File_SubBC
        pop bc
        xor a
        ld (VfsBuf),a                   ; статус: успех
        inc bc                          ; +1 байт статуса
        ld a,VFS_READ
        ld hl,VfsBuf
        call Proto_Send
        scf
        ret

; --- FILEX: сведения о томе -------------------------------------------------

Vfs_FsInfo:
        ld a,VFS_FSINFO
        ld (VfsCmd),a
        ld hl,(ProtoRxLen)
        ld de,1
        or a
        sbc hl,de
        jp nz,Vfs_Refuse
        call Vfs_FilexReset
        ld a,FILEX_OP_GET_FS_INFO
        ld (VfsFilexBlock+FILEX_P_OPERATION),a
        ld a,(ProtoBuf)
        and FILEX_FLAG_REFRESH_FREE
        ld (VfsFilexBlock+FILEX_P_FLAGS),a
        ld hl,VfsBuf+1
        ld (VfsFilexBlock+FILEX_P_BUFFER),hl
        ld hl,FILEX_FS_SIZE
        ld (VfsFilexBlock+FILEX_P_LENGTH),hl
        ld hl,VfsFilexBlock
        call WC_FILEX
        jr nz,.failed
        xor a
        ld (VfsBuf),a
        ld a,VFS_FSINFO
        ld hl,VfsBuf
        ld bc,FILEX_FS_SIZE+1
        call Proto_Send
        scf
        ret
.failed:
        ld c,a
        ld a,VFS_FSINFO
        jp Vfs_Reply1

; Потоковое чтение сырого окна выбранной активной FAT через FILEX op 7.
; Длина кратна 512 и ограничена 31 сектором: первые 32 байта page1 занимает
; блок FILEX, оставшиеся #3FE0 байт служат прямым буфером. Перед ответом Z80
; сжимает восемь FAT32-записей в один битовый байт карты занятости.
Vfs_FatWindow:
        ld a,VFS_FAT_WINDOW
        ld (VfsCmd),a
        ld hl,(ProtoRxLen)
        ld de,7
        or a
        sbc hl,de
        jp nz,Vfs_ReadWindowFail
        ld a,(ProtoBuf)
        ld hl,VfsFatSeq
        cp (hl)
        jp nz,Vfs_ReadWindowFail
        ld (VfsResponseSeq),a
        ld a,(ProtoBuf+1)
        or a
        jp nz,Vfs_ReadWindowFail
        ld a,(ProtoBuf+2)
        and 1
        jp nz,Vfs_ReadWindowFail
        ld hl,(ProtoBuf+5)
        ld a,h
        or l
        jp z,Vfs_ReadWindowFail
        ld a,l
        or a
        jp nz,Vfs_ReadWindowFail
        ld a,h
        and 1
        jp nz,Vfs_ReadWindowFail
        ld a,h
        cp #40
        jp nc,Vfs_ReadWindowFail
        ld (VfsReadWanted),hl
        ld hl,(ProtoBuf+1)
        ld (VfsFatOffset),hl
        ld hl,(ProtoBuf+3)
        ld (VfsFatOffset+2),hl
        call Vfs_FilexFatPage
        cp FILEX_STATUS_OK
        jr nz,.failed
        ld hl,(VfsFilexCount+2)
        ld a,h
        or l
        jr nz,.failed
        ld hl,(VfsFilexCount)
        ld de,(VfsReadWanted)
        or a
        sbc hl,de
        jr nz,.failed
        call Vfs_FatCompress
        ld hl,VFS_FILEX_DATA_C
        jp Vfs_ReadWindowBufferReady
.failed:
        ld c,a
        ld a,c
        or a
        jr nz,.status_ready
        ld c,VFS_FAIL
.status_ready:
        ld a,VFS_FAT_WINDOW
        jp Vfs_Reply1

; Сжать сырые FAT32-записи на месте: один выходной бит на один кластер.
; Старший служебный полубайт каждой записи не участвует в проверке занятости.
Vfs_FatCompress:
        push ix
        ld hl,VFS_FILEX_DATA_C
        ld ix,VFS_FILEX_DATA_C
        ld a,(VfsReadWanted+1)           ; сырая длина / 256
        ld l,a
        ld h,0
        add hl,hl                       ; сырая длина / 128
        add hl,hl                       ; сырая длина / 64
        add hl,hl                       ; сырая длина / 32 = байты карты
        ld (VfsReadCount),hl
        ld (VfsFatMapRemaining),hl
        ld hl,VFS_FILEX_DATA_C
.map_byte:
        ld c,0
        ld d,1
        ld b,8
.entry:
        ld a,(hl)
        inc hl
        ld e,a
        ld a,(hl)
        inc hl
        or e
        ld e,a
        ld a,(hl)
        inc hl
        or e
        ld e,a
        ld a,(hl)
        inc hl
        and #0F
        or e
        jr z,.next_entry
        ld a,c
        or d
        ld c,a
.next_entry:
        sla d
        djnz .entry
        ld (ix+0),c
        inc ix
        ld de,(VfsFatMapRemaining)
        dec de
        ld (VfsFatMapRemaining),de
        ld a,d
        or e
        jr nz,.map_byte
        pop ix
        ret

; Прочитать за один запрос до 16 КиБ. Wild Commander умеет LOAD512 с B=32,
; поэтому сначала заполняется вся page1, затем она уходит серией UART-кадров.
; Формат каждого ответа:
; Поля: [status][flags][seq][offset:2][total:2][crc16:2][data...].
Vfs_ReadWindow:
        ld a,VFS_READ_WINDOW
        ld (VfsCmd),a
        ld hl,(ProtoRxLen)
        ld de,3
        or a
        sbc hl,de
        jp nz,Vfs_ReadWindowFail
        ld a,(ProtoBuf)
        ld hl,VfsReadSeq
        cp (hl)
        jp nz,Vfs_ReadWindowFail
        ld (VfsResponseSeq),a
        ld hl,(ProtoBuf+1)
        call Vfs_Length16K
        jp c,Vfs_ReadWindowFail
        ld hl,(ProtoBuf+1)
        ld (VfsReadWanted),hl

        ld a,(VfsMode)
        cp 3
        jr nz,.sequential
        ld hl,(VfsReadWanted)
        call Vfs_LengthFilex
        jp c,Vfs_ReadWindowFail
        call Vfs_FilexReadPage
        cp FILEX_STATUS_OK
        jr z,.random_status_ok
        cp FILEX_STATUS_EOF
        jr z,.random_status_ok
        ; Быстрый вызов временно отдаёт #8000 второй странице плагина. Если
        ; конкретная конфигурация WC не сохраняет такой вызов, повторяем
        ; безопасное чтение через постоянный 512-байтный буфер code-page.
        ; READ_AT идемпотентен: резервный путь не меняет файл.
        call Vfs_FilexReadPageFallback
        cp FILEX_STATUS_OK
        jr z,.random_status_ok
        cp FILEX_STATUS_EOF
        jr nz,.random_failed
.random_status_ok:
        ld hl,(VfsFilexCount+2)
        ld a,h
        or l
        jr nz,.random_bad
        ld hl,(VfsFilexCount)
        ld de,(VfsReadWanted)
        or a
        sbc hl,de
        jr c,.random_count_ok
        jr z,.random_count_ok
.random_bad:
        ld a,VFS_FAIL
        jr .random_failed
.random_count_ok:
        ld hl,(VfsFilexCount)
        ld a,h
        or l
        jr z,.random_eof
        ld (VfsReadCount),hl
        call Vfs_AddRandomCount
        ld hl,VFS_FILEX_DATA_C
        jp Vfs_ReadWindowBufferReady
.random_eof:
        ld a,FILEX_STATUS_EOF
.random_failed:
        ld c,a
        ld a,VFS_READ_WINDOW
        jp Vfs_Reply1

.sequential:

        ld hl,FileRemaining
        call U32_IsZero
        jp z,Vfs_ReadWindowFail
        ; При ненулевом старшем слове остаток заведомо больше окна 16 КиБ.
        ld hl,(FileRemaining+2)
        ld a,h
        or l
        jr nz,.use_wanted
        ld hl,(FileRemaining)
        ld de,(VfsReadWanted)
        or a
        sbc hl,de
        jr nc,.use_wanted
        ld hl,(FileRemaining)            ; последний неполный блок файла
        jr .count_ready
.use_wanted:
        ld hl,(VfsReadWanted)
.count_ready:
        ld (VfsReadCount),hl

        ; Нефинальное окно обязано заканчиваться на границе сектора, иначе
        ; LOAD512 уже сдвинет поток дальше, чем отправлено в ESP.
        xor a
        ld (VfsReadFinal),a
        ld hl,(FileRemaining+2)
        ld a,h
        or l
        jr nz,.check_alignment
        ld hl,(FileRemaining)
        ld de,(VfsReadCount)
        or a
        sbc hl,de
        jr nz,.check_alignment
        ld a,1
        ld (VfsReadFinal),a
        jr .alignment_ok
.check_alignment:
        ld hl,(VfsReadCount)
        ld a,l
        or a
        jp nz,Vfs_ReadWindowFail
        ld a,h
        and 1
        jp nz,Vfs_ReadWindowFail
.alignment_ok:

        ; ceil(count / 512), допустимый диапазон B=1..32.
        ; После проверки финального окна HL содержит результат сравнения,
        ; поэтому фактическую длину обязательно загружаем заново.
        ld hl,(VfsReadCount)
        ld de,#01FF
        add hl,de
        ld a,h
        srl a
        ld (VfsReadSectors),a
        call Vfs_ReadWindowLoad
        jp c,Vfs_ReadWindowFail
        cp #0F                           ; физический конец цепочки
        jr nz,.loaded
        ld a,(VfsReadFinal)
        or a
        jp z,Vfs_ReadWindowFail          ; EOC раньше каталожного размера
.loaded:

        ld bc,(VfsReadCount)
        call File_SubBC
        ld hl,#C000
Vfs_ReadWindowBufferReady:
        ld (VfsReadPtr),hl
        ld bc,(VfsReadCount)
        call Vfs_Crc16
        ld (VfsReadCrc),de
        ld hl,0
        ld (VfsReadOffset),hl

.send:
        ld hl,(VfsReadCount)
        ld de,(VfsReadOffset)
        or a
        sbc hl,de                       ; HL = сколько осталось
        ld b,h
        ld c,l
        ld de,VFS_READ_DATA
        ld a,(VfsCmd)
        cp VFS_FAT_WINDOW
        jr nz,.part_limit_ready
        ld de,VFS_FAT_DATA
.part_limit_ready:
        or a
        sbc hl,de
        jr c,.part_ready
        jr z,.part_ready
        ld bc,VFS_READ_DATA
.part_ready:
        ld (VfsReadPart),bc
        ld hl,(VfsReadOffset)
        add hl,bc
        ld (VfsReadNewOffset),hl

        xor a
        ld (ProtoBuf),a                 ; нулевой статус
        ld (VfsReadFlags),a
        ld hl,(VfsReadOffset)
        ld a,h
        or l
        jr nz,.not_first
        ld a,VFS_WINDOW_START
        ld (VfsReadFlags),a
.not_first:
        ld hl,(VfsReadNewOffset)
        ld de,(VfsReadCount)
        or a
        sbc hl,de
        jr nz,.flags_ready
        ld a,(VfsReadFlags)
        or VFS_WINDOW_END
        ld (VfsReadFlags),a
.flags_ready:
        ld a,(VfsReadFlags)
        ld (ProtoBuf+1),a
        ld a,(VfsResponseSeq)
        ld (ProtoBuf+2),a
        ld hl,(VfsReadOffset)
        ld (ProtoBuf+3),hl
        ld hl,(VfsReadCount)
        ld (ProtoBuf+5),hl
        ld hl,(VfsReadCrc)
        ld (ProtoBuf+7),hl
        ld hl,(VfsReadPtr)
        ld de,ProtoBuf+VFS_READ_HEAD
        ld bc,(VfsReadPart)
        ldir
        ld (VfsReadPtr),hl

        ld hl,(VfsReadPart)
        ld de,VFS_READ_HEAD
        add hl,de
        ld b,h
        ld c,l
        ld a,(VfsCmd)
        ld hl,ProtoBuf
        call Proto_Send
        ret c
        ld hl,(VfsReadNewOffset)
        ld (VfsReadOffset),hl
        ld de,(VfsReadCount)
        or a
        sbc hl,de
        jp nz,.send
        ld a,(VfsCmd)
        cp VFS_FAT_WINDOW
        jr z,.advance_fat
        ld hl,VfsReadSeq
        jr .advance_sequence
.advance_fat:
        ld hl,VfsFatSeq
.advance_sequence:
        inc (hl)
        scf
        ret

Vfs_ReadWindowFail:
        ld a,(VfsCmd)
        ld c,VFS_FAIL
        jp Vfs_Reply1

; На время LOAD512 code-page отображается в #C000, а page1/data — в #8000.
; Трамплин использует только WC_API и переменные через alias +#4000.
Vfs_ReadWindowLoad:
        xor a
        call WC_MNGC_PL                  ; страница page0/code -> #C000
        jp Vfs_ReadWindowLoad_C + #4000

Vfs_ReadWindowLoad_C:
        ld a,1
        ex af,af'
        ld a,FN_MNG8_PL
        call WC_API                      ; страница page1/data -> #8000
        ld hl,#8000
        ld a,(VfsReadSectors+#4000)
        ld b,a
        ld a,FN_LOAD512
        call WC_API
        push af
        xor a
        ex af,af'
        ld a,FN_MNG8_PL
        call WC_API                      ; страница page0/code -> #8000
        pop af
        jp Vfs_ReadWindowLoad_Resume

Vfs_ReadWindowLoad_Resume:
        push af
        call Vfs_MapData                 ; page1/data обратно -> #C000
        pop af
        ret

; Вход HL=длина. CF=0 для 1..16 КиБ, CF=1 иначе.
Vfs_Length16K:
        ld a,h
        or l
        jr z,.bad
        ld de,VFS_APPEND_SIZE
        or a
        sbc hl,de
        jr c,.ok
        jr z,.ok
.bad:
        scf
        ret
.ok:
        or a
        ret

; Вход HL=длина. FILEX page1 оставляет первые 32 байта под parameter block.
Vfs_LengthFilex:
        ld a,h
        or l
        jr z,.bad
        ld de,VFS_FILEX_WINDOW
        or a
        sbc hl,de
        jr c,.ok
        jr z,.ok
.bad:
        scf
        ret
.ok:
        or a
        ret

; Старый raw WRITE намеренно не переиспользуется для нового формата. Благодаря
; отдельной команде #56 новая ESP не сможет записать LZ4-заголовок старому WMF
; как пользовательские данные.
Vfs_Write:
        ld a,VFS_WRITE
        ld (VfsCmd),a
        jp Vfs_Refuse

; Принять raw-окно до 16 КиБ без промежуточных подтверждений. Каждый кадр:
; [flags][seq][offset:2][total:2][crc16:2][data...]. CRC значим в END-кадре.
; Единственный ответ отправляется после проверки целого окна и APPEND полного
; блока #4000. Это убирает до 96 циклов запрос/ACK на каждые 16 КиБ.
Vfs_WriteWindow:
        ld a,VFS_WRITE_WINDOW
        ld (VfsCmd),a
        xor a
        ld (VfsReplySeq),a
        ld hl,(ProtoRxLen)
        ld de,2
        or a
        sbc hl,de
        jr c,.state
        ld a,(ProtoBuf+1)
        ld (VfsReplySeq),a
.state:
        ld a,(VfsMode)
        cp 1
        jr z,.write_mode
        cp 2
        jr z,.write_mode
        cp 3
        ld a,VFS_BS_STATE
        jp nz,Vfs_WriteWindowFail
.write_mode:
        ; Режим 1 создаёт новый пустой файл, а режим 2 снова открывает его для
        ; последовательного APPEND. Данные в обоих случаях приходят сюда одним
        ; и тем же оконным протоколом, поэтому оба режима являются записью.
        ld a,(VfsWriteFailed)
        or a
        jr z,.length
        scf                             ; первый отказ уже был отправлен
        ret
.length:
        ld hl,(ProtoRxLen)
        ld de,VFS_WINDOW_HEAD+1
        or a
        sbc hl,de
        ld a,VFS_BS_LENGTH
        jp c,Vfs_WriteWindowFail
        ld a,(ProtoBuf)
        ld (VfsWindowFlags),a
        and %11111100
        ld a,VFS_BS_LENGTH
        jp nz,Vfs_WriteWindowFail

        ld a,(VfsWindowFlags)
        and VFS_WINDOW_START
        jr z,.continuation
        ld a,(VfsWindowActive)
        or a
        ld a,VFS_BS_STATE
        jp nz,Vfs_WriteWindowFail
        ld a,(ProtoBuf+1)
        ld hl,VfsExpectedSeq
        cp (hl)
        ld a,VFS_BS_SEQ
        jp nz,Vfs_WriteWindowFail
        ld hl,(ProtoBuf+2)
        ld a,h
        or l
        ld a,VFS_BS_OFFSET
        jp nz,Vfs_WriteWindowFail
        ld hl,(ProtoBuf+4)
        ld a,(VfsMode)
        cp 3
        jr z,.filex_length
        call Vfs_Length16K
        jr .length_checked
.filex_length:
        call Vfs_LengthFilex
.length_checked:
        ld a,VFS_BS_LENGTH
        jp c,Vfs_WriteWindowFail
        ld hl,(ProtoBuf+4)
        ld (VfsWindowTotal),hl
        ld de,(VfsWriteCount)
        add hl,de
        jr c,.overflow
        ld de,VFS_APPEND_SIZE
        or a
        sbc hl,de
        jr c,.start_ok
        jr z,.start_ok
.overflow:
        ld a,VFS_BS_OVERFLOW
        jp Vfs_WriteWindowFail
.start_ok:
        ld a,(ProtoBuf+1)
        ld (VfsWindowSeq),a
        ld hl,0
        ld (VfsWindowReceived),hl
        ld a,1
        ld (VfsWindowActive),a
        jr .frame

.continuation:
        ld a,(VfsWindowActive)
        or a
        ld a,VFS_BS_STATE
        jp z,Vfs_WriteWindowFail
        ld a,(ProtoBuf+1)
        ld hl,VfsWindowSeq
        cp (hl)
        ld a,VFS_BS_SEQ
        jp nz,Vfs_WriteWindowFail
        ld hl,(ProtoBuf+4)
        ld de,(VfsWindowTotal)
        or a
        sbc hl,de
        ld a,VFS_BS_LENGTH
        jp nz,Vfs_WriteWindowFail

.frame:
        ld hl,(ProtoBuf+2)
        ld de,(VfsWindowReceived)
        or a
        sbc hl,de
        ld a,VFS_BS_OFFSET
        jp nz,Vfs_WriteWindowFail
        ld hl,(ProtoRxLen)
        ld de,VFS_WINDOW_HEAD
        or a
        sbc hl,de
        ld (VfsWindowPart),hl
        ld de,(VfsWindowReceived)
        add hl,de
        jp c,.bad_length
        ld (VfsWindowNewReceived),hl
        ld de,(VfsWindowTotal)
        or a
        sbc hl,de
        jp c,.check_end
        jp z,.check_end
.bad_length:
        ld a,VFS_BS_LENGTH
        jp Vfs_WriteWindowFail

.check_end:
        ld a,(VfsWindowFlags)
        and VFS_WINDOW_END
        jr z,.not_end
        ld hl,(VfsWindowNewReceived)
        ld de,(VfsWindowTotal)
        or a
        sbc hl,de
        ld a,VFS_BS_LENGTH
        jp nz,Vfs_WriteWindowFail
        jr .copy
.not_end:
        ld hl,(VfsWindowNewReceived)
        ld de,(VfsWindowTotal)
        or a
        sbc hl,de
        ld a,VFS_BS_LENGTH
        jp nc,Vfs_WriteWindowFail

.copy:
        call Vfs_MapData
        ld a,(VfsMode)
        cp 3
        jr z,.copy_filex
        ld hl,(VfsWriteCount)
        ld de,(VfsWindowReceived)
        add hl,de
        ld de,#C000
        jr .copy_address
.copy_filex:
        ld hl,(VfsWindowReceived)
        ld de,VFS_FILEX_DATA_C
.copy_address:
        add hl,de
        ex de,hl
        ld hl,ProtoBuf+VFS_WINDOW_HEAD
        ld bc,(VfsWindowPart)
        ldir
        ld hl,(VfsWindowNewReceived)
        ld (VfsWindowReceived),hl
        ld a,(VfsWindowFlags)
        and VFS_WINDOW_END
        jr nz,.finish
        scf                             ; кадр принят, ответа пока нет
        ret

.finish:
        ld a,(VfsMode)
        cp 3
        jr z,.crc_filex
        ld hl,(VfsWriteCount)
        ld de,#C000
        add hl,de
        jr .crc_ready
.crc_filex:
        ld hl,VFS_FILEX_DATA_C
.crc_ready:
        ld bc,(VfsWindowTotal)
        call Vfs_Crc16
        ld hl,(ProtoBuf+6)
        or a
        sbc hl,de
        ld a,VFS_BS_CRC
        jp nz,Vfs_WriteWindowFail
        ld a,(VfsMode)
        cp 3
        jr z,.store_filex
        ld hl,(VfsWriteCount)
        ld de,(VfsWindowTotal)
        add hl,de
        ld (VfsWriteCount),hl
        xor a
        ld (VfsWindowActive),a
        ld hl,VfsExpectedSeq
        inc (hl)
        ld hl,(VfsWriteCount)
        ld de,VFS_APPEND_SIZE
        or a
        sbc hl,de
        jr nz,.ok
        call Vfs_WriteFlush
        jr z,.ok
        ld a,VFS_BS_APPEND
        jp Vfs_WriteWindowFail
.store_filex:
        call Vfs_FilexWritePage
        or a
        jp nz,Vfs_WriteWindowFail
        ld hl,(VfsFilexCount+2)
        ld a,h
        or l
        ld a,VFS_BS_APPEND
        jp nz,Vfs_WriteWindowFail
        ld hl,(VfsFilexCount)
        ld de,(VfsWindowTotal)
        or a
        sbc hl,de
        ld a,VFS_BS_APPEND
        jp nz,Vfs_WriteWindowFail
        call Vfs_AddRandomCount
        xor a
        ld (VfsWindowActive),a
        ld hl,VfsExpectedSeq
        inc (hl)
.ok:
        xor a
        jp Vfs_WriteWindowReply

Vfs_WriteWindowFail:
        push af
        ld a,1
        ld (VfsWriteFailed),a
        xor a
        ld (VfsWindowActive),a
        pop af

Vfs_WriteWindowReply:
        ld (VfsBuf),a
        ld a,(VfsReplySeq)
        ld (VfsBuf+1),a
        ld hl,(VfsWindowReceived)
        ld (VfsBuf+2),hl
        ld a,VFS_WRITE_WINDOW
        ld hl,VfsBuf
        ld bc,4
        call Proto_Send
        scf
        ret

; Принять первый либо очередной фрагмент независимого блока до 512 байт.
; Первый payload имеет поля:
;   [kind: 00 RAW/01 LZ4][seq][raw_len:2][stored_len:2][crc16:2][data...], формат блока
; Продолжение:
;   [80][seq][offset:2][data...], формат следующего фрагмента
Vfs_Block:
        ld a,VFS_BLOCK
        ld (VfsCmd),a
        xor a
        ld (VfsReplySeq),a

        ld a,(VfsMode)
        cp 1
        jr z,.write_mode
        cp 2
        ld a,VFS_BS_STATE
        jp nz,Vfs_BlockFail
.write_mode:
        ; Это запасной блочный тракт для старой ESP без WRITE_WINDOW. Он обязан
        ; принимать те же два режима записи, что и основной 16-КиБ тракт.
        ld a,(VfsWriteFailed)
        or a
        ld a,VFS_BS_STATE
        jp nz,Vfs_BlockFail

        ; Полный payload обязан быть 1..256 байт.
        ld hl,(ProtoRxLen)
        ld a,h
        or l
        ld a,VFS_BS_LENGTH
        jp z,Vfs_BlockFail
        ld hl,(ProtoRxLen)
        ld de,VFS_WIRE_SIZE
        or a
        sbc hl,de
        jp c,.wire_ok
        jp z,.wire_ok
        ld a,VFS_BS_LENGTH
        jp Vfs_BlockFail
.wire_ok:
        ld hl,(ProtoRxLen)
        ld de,2
        or a
        sbc hl,de
        jr c,.kind
        ld a,(ProtoBuf+1)
        ld (VfsReplySeq),a
.kind:
        ld a,(ProtoBuf)
        cp #80
        jp z,.continuation
        or a
        jp z,.first
        cp 1
        jp z,.first
        ld a,VFS_BS_LENGTH
        jp Vfs_BlockFail

.first:
        ld hl,(ProtoRxLen)
        ld de,VFS_BLOCK_HEAD+1
        or a
        sbc hl,de
        ld a,VFS_BS_LENGTH
        jp c,Vfs_BlockFail
        ld a,(VfsBlockActive)
        or a
        ld a,VFS_BS_STATE
        jp nz,Vfs_BlockFail
        ld a,(ProtoBuf+1)
        ld hl,VfsExpectedSeq
        cp (hl)
        ld a,VFS_BS_SEQ
        jp nz,Vfs_BlockFail

        ld hl,(ProtoBuf+2)
        ld (VfsBlockRawLen),hl
        call Vfs_Length512
        ld a,VFS_BS_LENGTH
        jp c,Vfs_BlockFail
        ld hl,(ProtoBuf+4)
        ld (VfsBlockStoredLen),hl
        call Vfs_Length512
        ld a,VFS_BS_LENGTH
        jp c,Vfs_BlockFail

        ld a,(ProtoBuf)
        ld (VfsBlockKind),a
        or a
        jr nz,.lz_lengths
        ld hl,(VfsBlockStoredLen)
        ld de,(VfsBlockRawLen)
        or a
        sbc hl,de
        ld a,VFS_BS_LENGTH
        jp nz,Vfs_BlockFail
        jr .lengths_ok
.lz_lengths:
        ; LZ4 используется только когда stored_len строго меньше raw_len.
        ld hl,(VfsBlockStoredLen)
        ld de,(VfsBlockRawLen)
        or a
        sbc hl,de
        ld a,VFS_BS_LENGTH
        jp nc,Vfs_BlockFail
.lengths_ok:
        ld a,(ProtoBuf+1)
        ld (VfsBlockSeq),a
        ld hl,(ProtoBuf+6)
        ld (VfsBlockCrc),hl
        ld hl,(ProtoRxLen)
        ld de,VFS_BLOCK_HEAD
        or a
        sbc hl,de
        ld b,h
        ld c,l                           ; BC = данные первого фрагмента
        ld hl,(VfsBlockStoredLen)
        or a
        sbc hl,bc
        ld a,VFS_BS_LENGTH
        jp c,Vfs_BlockFail
        ld hl,ProtoBuf+VFS_BLOCK_HEAD
        ld de,VfsCompBuf
        ldir
        ld (VfsBlockReceived),bc         ; LDIR оставил BC=0
        ld hl,(ProtoRxLen)
        ld de,VFS_BLOCK_HEAD
        or a
        sbc hl,de
        ld (VfsBlockReceived),hl
        ld a,1
        ld (VfsBlockActive),a
        jp Vfs_BlockMaybeFinish

.continuation:
        ld hl,(ProtoRxLen)
        ld de,VFS_CONT_HEAD+1
        or a
        sbc hl,de
        ld a,VFS_BS_LENGTH
        jp c,Vfs_BlockFail
        ld a,(VfsBlockActive)
        or a
        ld a,VFS_BS_STATE
        jp z,Vfs_BlockFail
        ld a,(ProtoBuf+1)
        ld hl,VfsBlockSeq
        cp (hl)
        ld a,VFS_BS_SEQ
        jp nz,Vfs_BlockFail
        ld hl,(ProtoBuf+2)
        ld de,(VfsBlockReceived)
        or a
        sbc hl,de
        ld a,VFS_BS_OFFSET
        jp nz,Vfs_BlockFail

        ld hl,(ProtoRxLen)
        ld de,VFS_CONT_HEAD
        or a
        sbc hl,de
        ld (VfsBlockPart),hl
        ld de,(VfsBlockReceived)
        add hl,de
        jp c,.cont_length
        ld (VfsBlockNewReceived),hl
        ld de,(VfsBlockStoredLen)
        or a
        sbc hl,de
        jp c,.cont_copy
        jp z,.cont_copy
.cont_length:
        ld a,VFS_BS_LENGTH
        jp Vfs_BlockFail
.cont_copy:
        ld hl,VfsCompBuf
        ld de,(VfsBlockReceived)
        add hl,de
        ex de,hl
        ld hl,ProtoBuf+VFS_CONT_HEAD
        ld bc,(VfsBlockPart)
        ldir
        ld hl,(VfsBlockNewReceived)
        ld (VfsBlockReceived),hl
        jp Vfs_BlockMaybeFinish

; Вход HL=длина. CF=0 для 1..512, CF=1 иначе.
Vfs_Length512:
        ld a,h
        or l
        jr z,.bad
        ld de,VFS_BLOCK_SIZE
        or a
        sbc hl,de
        jr c,.ok
        jr z,.ok
.bad:
        scf
        ret
.ok:
        or a
        ret

Vfs_BlockMaybeFinish:
        ld hl,(VfsBlockReceived)
        ld de,(VfsBlockStoredLen)
        or a
        sbc hl,de
        jr nz,.ack
        call Vfs_BlockDecode
        or a
        jp nz,Vfs_BlockFail
        xor a
        ld (VfsBlockActive),a
        ld hl,VfsExpectedSeq
        inc (hl)
.ack:
        xor a
        jp Vfs_BlockReply

; Распаковать/скопировать полностью принятый блок в page1:#C000, проверить
; CRC-16/CCITT-FALSE и при заполнении 16 КиБ выполнить один APPEND.
; Выход: A=0 либо VFS_BS_*.
Vfs_BlockDecode:
        ld hl,(VfsWriteCount)
        ld de,(VfsBlockRawLen)
        add hl,de
        jr c,.overflow
        ld (VfsBlockNewCount),hl
        ld de,VFS_APPEND_SIZE
        or a
        sbc hl,de
        jr c,.capacity_ok
        jr z,.capacity_ok
.overflow:
        ld a,VFS_BS_OVERFLOW
        ret
.capacity_ok:
        ld hl,(VfsWriteCount)
        ld de,#C000
        add hl,de
        ld (VfsOutStart),hl
        ld (VfsLzOutPtr),hl
        call Vfs_MapData

        ld a,(VfsBlockKind)
        or a
        jr nz,.lz4
        ld hl,VfsCompBuf
        ld de,(VfsOutStart)
        ld bc,(VfsBlockRawLen)
        ldir
        jr .crc
.lz4:
        call Vfs_Lz4Decode
        jr nc,.crc
        ld a,VFS_BS_LZ4
        ret
.crc:
        ld hl,(VfsOutStart)
        ld bc,(VfsBlockRawLen)
        call Vfs_Crc16
        ld hl,(VfsBlockCrc)
        or a
        sbc hl,de
        jr z,.verified
        ld a,VFS_BS_CRC
        ret
.verified:
        ld hl,(VfsBlockNewCount)
        ld (VfsWriteCount),hl
        ld de,VFS_APPEND_SIZE
        or a
        sbc hl,de
        jr nz,.ok
        call Vfs_WriteFlush
        jr z,.ok
        ld a,VFS_BS_APPEND
        ret
.ok:
        xor a
        ret

; Стандартный LZ4 Block decoder без внешнего словаря. Все offset обязаны
; ссылаться только на уже распакованные байты текущего блока.
; Выход: CF=0 успех, CF=1 битый поток.
Vfs_Lz4Decode:
        ld hl,VfsCompBuf
        ld (VfsLzSrcPtr),hl
        ld hl,(VfsBlockStoredLen)
        ld (VfsLzSrcLeft),hl
        ld hl,(VfsBlockRawLen)
        ld (VfsLzOutLeft),hl
        ld hl,0
        ld (VfsLzProduced),hl
.sequence:
        call Vfs_LzGetByte
        jp c,.bad
        ld (VfsLzToken),a
        and #F0
        rrca
        rrca
        rrca
        rrca
        call Vfs_LzLength
        jp c,.bad
        call Vfs_LzCopyLiterals
        jp c,.bad

        ld hl,(VfsLzSrcLeft)
        ld a,h
        or l
        jr nz,.match
        ld hl,(VfsLzOutLeft)
        ld a,h
        or l
        jr nz,.bad
        or a
        ret

.match:
        call Vfs_LzGetByte
        jr c,.bad
        ld e,a
        call Vfs_LzGetByte
        jr c,.bad
        ld d,a
        ld a,d
        or e
        jr z,.bad
        ld (VfsLzOffset),de
        ld hl,(VfsLzProduced)
        or a
        sbc hl,de
        jr c,.bad

        ld a,(VfsLzToken)
        and #0F
        call Vfs_LzLength
        jr c,.bad
        ld h,b
        ld l,c
        ld de,4
        add hl,de
        jr c,.bad
        ld b,h
        ld c,l
        ld (VfsLzLength),bc
        ld hl,(VfsLzOutLeft)
        or a
        sbc hl,bc
        jr c,.bad
        ld (VfsLzOutLeft),hl
        ld hl,(VfsLzProduced)
        add hl,bc
        jr c,.bad
        ld (VfsLzProduced),hl

        ld hl,(VfsLzOutPtr)
        ld de,(VfsLzOffset)
        or a
        sbc hl,de
        ld de,(VfsLzOutPtr)
        ld bc,(VfsLzLength)
        ldir                              ; forward copy корректен при overlap
        ld (VfsLzOutPtr),de
        jp .sequence
.bad:
        scf
        ret

; Следующий байт compressed source. CF=1 при преждевременном конце.
Vfs_LzGetByte:
        ld hl,(VfsLzSrcLeft)
        ld a,h
        or l
        jr z,.empty
        dec hl
        ld (VfsLzSrcLeft),hl
        ld hl,(VfsLzSrcPtr)
        ld a,(hl)
        inc hl
        ld (VfsLzSrcPtr),hl
        or a
        ret
.empty:
        scf
        ret

; Декодировать nibble длины LZ4 с extension-байтами. Вход A=0..15,
; выход BC=длина, CF=1 при обрыве/переполнении.
Vfs_LzLength:
        ld c,a
        ld b,0
        cp 15
        jr nz,.done
.more:
        call Vfs_LzGetByte
        ret c
        ld e,a
        ld d,0
        ld h,b
        ld l,c
        add hl,de
        jr c,.bad
        ld b,h
        ld c,l
        ld a,e
        cp #FF
        jr z,.more
.done:
        or a
        ret
.bad:
        scf
        ret

; Скопировать BC literals, обновив source/output/produced.
Vfs_LzCopyLiterals:
        ld (VfsLzLength),bc
        ld hl,(VfsLzSrcLeft)
        or a
        sbc hl,bc
        jr c,.bad
        ld (VfsLzSrcLeft),hl
        ld hl,(VfsLzOutLeft)
        or a
        sbc hl,bc
        jr c,.bad
        ld (VfsLzOutLeft),hl
        ld hl,(VfsLzProduced)
        add hl,bc
        jr c,.bad
        ld (VfsLzProduced),hl
        ld a,b
        or c
        jr z,.done
        ld hl,(VfsLzSrcPtr)
        ld de,(VfsLzOutPtr)
        ldir
        ld (VfsLzSrcPtr),hl
        ld (VfsLzOutPtr),de
.done:
        or a
        ret
.bad:
        scf
        ret

; CRC-16/CCITT-FALSE: poly #1021, init #FFFF. Вход HL/BC, выход DE.
Vfs_Crc16:
        ld de,#FFFF
.byte:
        ld a,b
        or c
        ret z
        ld a,d
        xor (hl)
        ld d,a
        inc hl
        push bc
        ld b,8
.bit:
        sla e
        rl d
        jr nc,.next
        ld a,d
        xor #10
        ld d,a
        ld a,e
        xor #21
        ld e,a
.next:
        djnz .bit
        pop bc
        dec bc
        jr .byte

; Page1 всегда заново отображается перед записью: UI/API могли сменить #C000.
Vfs_MapData:
        ld a,1
        jp WC_MNGC_PL

; FILEX block живёт в первых 32 байтах page1, данные — сразу после него.
; Это оставляет одно позиционное окно #3FE0 байт без копирования на ESP.
Vfs_FilexPageReset:
        call Vfs_MapData
        ld hl,#C000
        ld de,#C001
        ld bc,FILEX_BLOCK_SIZE-1
        xor a
        ld (hl),a
        ldir
        ld a,FILEX_BLOCK_SIZE
        ld (#C000),a
        ld a,FILEX_API_VERSION
        ld (#C001),a
        ret

Vfs_FilexReadPage:
        call Vfs_FilexPageReset
        ld a,FILEX_OP_READ_AT
        ld (#C000+FILEX_P_OPERATION),a
        ld hl,(VfsRandomOffset)
        ld (#C000+FILEX_P_OFFSET),hl
        ld hl,(VfsRandomOffset+2)
        ld (#C000+FILEX_P_OFFSET+2),hl
        ld hl,VFS_FILEX_DATA
        ld (#C000+FILEX_P_BUFFER),hl
        ld hl,(VfsReadWanted)
        ld (#C000+FILEX_P_LENGTH),hl
        jr Vfs_FilexPageCall

Vfs_FilexFatPage:
        call Vfs_FilexPageReset
        ld a,FILEX_OP_READ_FAT
        ld (#C000+FILEX_P_OPERATION),a
        ld hl,(VfsFatOffset)
        ld (#C000+FILEX_P_OFFSET),hl
        ld hl,(VfsFatOffset+2)
        ld (#C000+FILEX_P_OFFSET+2),hl
        ld hl,VFS_FILEX_DATA
        ld (#C000+FILEX_P_BUFFER),hl
        ld hl,(VfsReadWanted)
        ld (#C000+FILEX_P_LENGTH),hl
        jr Vfs_FilexPageCall

Vfs_FilexWritePage:
        call Vfs_FilexPageReset
        ld a,FILEX_OP_WRITE_AT
        ld (#C000+FILEX_P_OPERATION),a
        ld hl,(VfsRandomOffset)
        ld (#C000+FILEX_P_OFFSET),hl
        ld hl,(VfsRandomOffset+2)
        ld (#C000+FILEX_P_OFFSET+2),hl
        ld hl,VFS_FILEX_DATA
        ld (#C000+FILEX_P_BUFFER),hl
        ld hl,(VfsWindowTotal)
        ld (#C000+FILEX_P_LENGTH),hl

Vfs_FilexPageCall:
        xor a
        call WC_MNGC_PL                  ; страница кода -> #C000
        jp Vfs_FilexPageCall_C + #4000

Vfs_FilexPageCall_C:
        ld a,1
        ex af,af'
        ld a,FN_MNG8_PL
        call WC_API                      ; страница блока и данных -> #8000
        ld hl,#8000
        ld a,FN_FILEX
        call WC_API
        ld (VfsFilexStatus+#4000),a
        ld hl,(#8000+FILEX_P_RESULT_COUNT)
        ld (VfsFilexCount+#4000),hl
        ld hl,(#8000+FILEX_P_RESULT_COUNT+2)
        ld (VfsFilexCount+#4002),hl
        xor a
        ex af,af'
        ld a,FN_MNG8_PL
        call WC_API                      ; страница кода -> #8000
        jp Vfs_FilexPageCall_Resume

Vfs_FilexPageCall_Resume:
        call Vfs_MapData                 ; page1/data обратно -> #C000
        ld a,(VfsFilexStatus)
        or a
        ret

; Резервное позиционное чтение без замены code-page в окне #8000. Каждый
; READ_AT принимает не больше одного сектора в VfsBuf, после чего данные
; переносятся в page1. Это медленнее единого окна, но сохраняет прямое
; 32-битное смещение и никогда не проходит содержимое файла от его начала.
; Выход совпадает с Vfs_FilexReadPage: A=status, VfsFilexCount=прочитано.
Vfs_FilexReadPageFallback:
        call Vfs_MapData
        ld hl,VFS_FILEX_DATA_C
        ld (VfsFilexStagePtr),hl
        ld hl,(VfsReadWanted)
        ld (VfsFilexRemaining),hl
        ld hl,0
        ld (VfsFilexCount),hl
        ld (VfsFilexCount+2),hl
        ld hl,(VfsRandomOffset)
        ld (VfsFilexWorkOffset),hl
        ld hl,(VfsRandomOffset+2)
        ld (VfsFilexWorkOffset+2),hl
.next:
        ld hl,(VfsFilexRemaining)
        ld a,h
        or l
        jp z,.complete
        ld de,IO_SIZE
        or a
        sbc hl,de
        jr nc,.full_sector
        ld bc,(VfsFilexRemaining)
        jr .chunk_ready
.full_sector:
        ld bc,IO_SIZE
.chunk_ready:
        ld (VfsFilexChunk),bc
        call Vfs_FilexReset
        ld a,FILEX_OP_READ_AT
        ld (VfsFilexBlock+FILEX_P_OPERATION),a
        ld hl,(VfsFilexWorkOffset)
        ld (VfsFilexBlock+FILEX_P_OFFSET),hl
        ld hl,(VfsFilexWorkOffset+2)
        ld (VfsFilexBlock+FILEX_P_OFFSET+2),hl
        ld hl,VfsBuf
        ld (VfsFilexBlock+FILEX_P_BUFFER),hl
        ld bc,(VfsFilexChunk)
        ld (VfsFilexBlock+FILEX_P_LENGTH),bc
        ld hl,VfsFilexBlock
        call WC_FILEX
        ld (VfsFilexStatus),a
        cp FILEX_STATUS_OK
        jr z,.status_ready
        cp FILEX_STATUS_EOF
        jr nz,.failed
.status_ready:
        ld hl,(VfsFilexBlock+FILEX_P_RESULT_COUNT+2)
        ld a,h
        or l
        jr nz,.internal
        ld hl,(VfsFilexBlock+FILEX_P_RESULT_COUNT)
        ld (VfsFilexPart),hl
        ld a,h
        or l
        jr z,.internal
        ld de,(VfsFilexChunk)
        or a
        sbc hl,de
        jr z,.count_ready
        jr nc,.internal
        ld a,(VfsFilexStatus)
        cp FILEX_STATUS_EOF
        jr nz,.internal
.count_ready:
        ld hl,VfsBuf
        ld de,(VfsFilexStagePtr)
        ld bc,(VfsFilexPart)
        ldir
        ld (VfsFilexStagePtr),de

        ld hl,(VfsFilexCount)
        ld de,(VfsFilexPart)
        add hl,de
        ld (VfsFilexCount),hl
        ld hl,(VfsFilexRemaining)
        or a
        sbc hl,de
        ld (VfsFilexRemaining),hl
        ld hl,(VfsFilexWorkOffset)
        add hl,de
        ld (VfsFilexWorkOffset),hl
        ld hl,(VfsFilexWorkOffset+2)
        ld de,0
        adc hl,de
        ld (VfsFilexWorkOffset+2),hl
        ld a,(VfsFilexStatus)
        cp FILEX_STATUS_EOF
        jp nz,.next
        ret
.complete:
        xor a
        ret
.internal:
        ld a,FILEX_STATUS_INTERNAL
.failed:
        or a
        ret

Vfs_AddRandomCount:
        ld hl,(VfsRandomOffset)
        ld de,(VfsFilexCount)
        add hl,de
        ld (VfsRandomOffset),hl
        ld hl,(VfsRandomOffset+2)
        ld de,(VfsFilexCount+2)
        adc hl,de
        ld (VfsRandomOffset+2),hl
        ret

; Опубликовать накопленные 1..16 КиБ одним APPEND. Page1 сначала переносится из
; #C000 в допустимое для API 76 окно #8000. Пока code-page отсутствует в #8000,
; трамплин исполняется из её alias в #C000 и вызывает только фиксированный API.
; Выход: Z — записано/буфер пуст; NZ — ошибка, повтор блока запрещён.
Vfs_WriteFlush:
        ld bc,(VfsWriteCount)
        ld a,b
        or c
        ret z
        xor a
        call WC_MNGC_PL                   ; страница page0/code -> #C000
        jp Vfs_WriteFlush_C + #4000

; Один APPEND на весь накопленный буфер. Дробление на куски меньше сектора
; здесь было — как обход подозрения на порчу при многоблочной записи. Подозрение
; снято проверкой на железе: 20000, 40000 и 80000 байт записались и прочитались
; обратно побайтово, а скорость с кусками и без них одинаковая, потому что
; межвызовный LBA-кэш CORE32 убрал стоимость повторных дописываний. Оставлен
; штатный путь: меньше вызовов API и меньше кода.
Vfs_WriteFlush_C:
        ld a,1
        ex af,af'
        ld a,FN_MNG8_PL
        call WC_API                       ; страница page1/data -> #8000
        ld hl,#8000
        ld bc,(VfsWriteCount+#4000)       ; переменная в alias страницы page0
        ld a,FN_APPEND
        call WC_API                       ; wrapper #8xxx сейчас недоступен
        push af
        xor a
        ex af,af'
        ld a,FN_MNG8_PL
        call WC_API                       ; страница page0/code -> #8000
        pop af
        jp Vfs_WriteFlush_Resume

Vfs_WriteFlush_Resume:
        push af
        call Vfs_MapData                  ; page1/data обратно -> #C000
        pop af
        jr z,.stored
        ld a,1
        ld (VfsWriteFailed),a
        or a
        ret
.stored:
        ld hl,0
        ld (VfsWriteCount),hl
        xor a
        ret

Vfs_WriteReset:
        ld hl,0
        ld (VfsWriteCount),hl
        ld (VfsBlockReceived),hl
        ld (VfsBlockRawLen),hl
        ld (VfsBlockStoredLen),hl
        ld (VfsWindowReceived),hl
        ld (VfsWindowTotal),hl
        ld (VfsReadOffset),hl
        xor a
        ld (VfsWriteFailed),a
        ld (VfsBlockActive),a
        ld (VfsWindowActive),a
        ld (VfsExpectedSeq),a
        ld (VfsBlockSeq),a
        ld (VfsWindowSeq),a
        ld (VfsReadSeq),a
        ret

; Дописать к уже найденному файлу указанное 32-битное число нулей. Команда
; вызывается только в свежем OPEN=2: это гарантирует, что заполнение page1
; нулями не уничтожит ещё не опубликованный хвост обычной WRITE_WINDOW.
; Нули формируются локально на Z80 и не занимают UART.
Vfs_Extend:
        ld a,VFS_EXTEND
        ld (VfsCmd),a
        ld hl,(ProtoRxLen)
        ld de,4
        or a
        sbc hl,de
        jp nz,Vfs_Refuse
        ld a,(VfsMode)
        cp 2
        jp nz,Vfs_Refuse
        ld a,(VfsWriteFailed)
        or a
        jp nz,Vfs_Refuse
        ld hl,(VfsWriteCount)
        ld a,h
        or l
        jp nz,Vfs_Refuse
        ld a,(VfsBlockActive)
        or a
        jp nz,Vfs_Refuse
        ld a,(VfsWindowActive)
        or a
        jp nz,Vfs_Refuse

        ld hl,(ProtoBuf)
        ld (VfsExtendRemaining),hl
        ld hl,(ProtoBuf+2)
        ld (VfsExtendRemaining+2),hl
        ld a,h
        or l
        jr nz,.prepare
        ld hl,(VfsExtendRemaining)
        ld a,h
        or l
        jr z,.success

.prepare:
        call Vfs_MapData
        xor a
        ld hl,#C000
        ld (hl),a
        ld de,#C001
        ld bc,VFS_APPEND_SIZE-1
        ldir

.next:
        ld hl,(VfsExtendRemaining+2)
        ld a,h
        or l
        jr nz,.full
        ld hl,(VfsExtendRemaining)
        ld a,h
        or l
        jr z,.success
        ld de,VFS_APPEND_SIZE
        or a
        sbc hl,de
        jr nc,.full
        ld bc,(VfsExtendRemaining)
        jr .write
.full:
        ld bc,VFS_APPEND_SIZE
.write:
        push bc
        ld (VfsWriteCount),bc
        call Vfs_WriteFlush
        pop bc
        jr nz,.failed

        ld hl,(VfsExtendRemaining)
        or a
        sbc hl,bc
        ld (VfsExtendRemaining),hl
        ld hl,(VfsExtendRemaining+2)
        ld bc,0
        sbc hl,bc
        ld (VfsExtendRemaining+2),hl
        jr .next

.failed:
        ld a,VFS_EXTEND
        ld c,VFS_FAIL
        jp Vfs_Reply1
.success:
        ld a,VFS_EXTEND
        ld c,VFS_OK
        jp Vfs_Reply1

; Изменить абсолютную позицию последующих READ_WINDOW/WRITE_WINDOW FILEX.
Vfs_Seek:
        ld a,VFS_SEEK
        ld (VfsCmd),a
        ld hl,(ProtoRxLen)
        ld de,4
        or a
        sbc hl,de
        jp nz,Vfs_Refuse
        ld a,(VfsMode)
        cp 3
        jp nz,Vfs_Refuse
        ld hl,(ProtoBuf)
        ld (VfsRandomOffset),hl
        ld hl,(ProtoBuf+2)
        ld (VfsRandomOffset+2),hl
        ld a,VFS_SEEK
        ld c,VFS_OK
        jp Vfs_Reply1

; Установить абсолютный EOF найденного OPEN=3 файла.
Vfs_SetEof:
        ld a,VFS_SET_EOF
        ld (VfsCmd),a
        ld hl,(ProtoRxLen)
        ld de,4
        or a
        sbc hl,de
        jp nz,Vfs_Refuse
        ld a,(VfsMode)
        cp 3
        jp nz,Vfs_Refuse
        call Vfs_FilexReset
        ld a,FILEX_OP_SET_EOF32
        ld (VfsFilexBlock+FILEX_P_OPERATION),a
        ld hl,(ProtoBuf)
        ld (VfsFilexBlock+FILEX_P_OFFSET),hl
        ld hl,(ProtoBuf+2)
        ld (VfsFilexBlock+FILEX_P_OFFSET+2),hl
        ld hl,VfsFilexBlock
        call WC_FILEX
        cp FILEX_STATUS_OK
        jr z,.answer
        cp FILEX_STATUS_COMMITTED_CLEANUP
        jr z,.answer
        ld c,a
        ld a,VFS_SET_EOF
        jp Vfs_Reply1
.answer:
        ld (VfsBuf),a                   ; #25 остаётся видимым ESP
        ld hl,VfsFilexBlock+FILEX_P_RESULT_COUNT
        ld de,VfsBuf+1
        ld bc,4
        ldir
        ld a,VFS_SET_EOF
        ld hl,VfsBuf
        ld bc,5
        call Proto_Send
        scf
        ret

Vfs_SetMetadata:
        ld a,VFS_SET_METADATA
        ld (VfsCmd),a
        ld hl,(ProtoRxLen)
        ld de,FILEX_META_SIZE
        or a
        sbc hl,de
        jp nz,Vfs_Refuse
        ld a,(VfsMode)
        cp 3
        jp nz,Vfs_Refuse
        ld hl,ProtoBuf
        ld de,VfsMetaBuffer
        ld bc,FILEX_META_SIZE
        ldir
        call Vfs_FilexReset
        ld a,FILEX_OP_SET_METADATA
        ld (VfsFilexBlock+FILEX_P_OPERATION),a
        ld hl,VfsMetaBuffer
        ld (VfsFilexBlock+FILEX_P_BUFFER),hl
        ld hl,FILEX_META_SIZE
        ld (VfsFilexBlock+FILEX_P_LENGTH),hl
        ld hl,VfsFilexBlock
        call WC_FILEX
        jr nz,.failed
        xor a
        ld (VfsBuf),a
        ld a,(VfsMetaBuffer+15)
        ld (VfsBuf+1),a
        ld a,VFS_SET_METADATA
        ld hl,VfsBuf
        ld bc,2
        call Proto_Send
        scf
        ret
.failed:
        ld c,a
        ld a,VFS_SET_METADATA
        jp Vfs_Reply1

Vfs_Close:
        ld a,VFS_CLOSE
        ld (VfsCmd),a
        ld c,VFS_OK
        ld a,(VfsMode)
        cp 1
        jr z,.write_mode
        cp 2
        jr nz,.reset
.write_mode:

        ; Пустой payload сохраняет старый контракт и означает commit.
        ; Payload [0] — abort: остаток не пишется, частичный файл удалит ESP.
        ld hl,(ProtoRxLen)
        ld a,h
        or l
        jr z,.commit
        ld a,(ProtoBuf)
        or a
        jr z,.reset
.commit:
        ld a,(VfsWriteFailed)
        or a
        jr nz,.failed
        ld a,(VfsBlockActive)
        or a
        jr nz,.failed
        ld a,(VfsWindowActive)
        or a
        jr nz,.failed                    ; незавершённое окно нельзя публиковать
        call Vfs_WriteFlush              ; последний фактический остаток
        jr nz,.failed
        ; APPEND/WC_API вправе менять BC. После успешного flush нельзя
        ; отправлять его случайный C как статус CLOSE (на железе пришёл #AF).
        ld c,VFS_OK
        jr .reset
.failed:
        ld c,VFS_FAIL
.reset:
        push bc
        call Vfs_WriteReset
        xor a
        ld (VfsMode),a
        ld hl,0
        ld (FileRemaining),hl
        ld (FileRemaining+2),hl
        pop bc
        ld a,VFS_CLOSE
        jp Vfs_Reply1

Vfs_Delete:
        ld a,VFS_DELETE
        ld (VfsCmd),a
        xor a
        call Vfs_Locate
        jp c,Vfs_Refuse
        jp z,Vfs_Refuse                 ; корень удалять нельзя
        call Fs_SelectWork
        ld hl,FsEntry
        call WC_FENTRY
        jr nz,.entry_found
        ; Атрибут 0 ищет обычный файл. Если файл не найден, повторить поиск
        ; как каталог: API FENTRY требует бит каталога #10 даже для DELETE.
        ld a,#10
        ld (FsEntry),a
        call Fs_SelectWork
        ld hl,FsEntry
        call WC_FENTRY
        jp z,Vfs_Refuse
        ; WC_DELETE сам не проверяет пустоту каталога. Войти в найденный
        ; каталог и убедиться, что FINDNEXT не возвращает ни одного объекта.
        ; Затем обязательно восстановить родительский каталог для DELETE.
        call WC_GDIR
.check_directory:
        call Fs_SelectWork
        ld de,DirEntryBuffer
        ld a,#1C
        call WC_FINDNEXT
        jr z,.directory_empty
        ld hl,DirEntryBuffer+9
        ld de,PathDot
        call String_Equals
        jr z,.check_directory
        ld hl,DirEntryBuffer+9
        ld de,PathDotDot
        call String_Equals
        jr z,.check_directory
        ld a,1                         ; NZ: найден реальный объект
        or a
.directory_empty:
        push af
        call Fs_ResetRoot
        ld hl,VfsPath
        call Fs_ChangePath
        jr c,.restore_failed
        ld a,#10
        ld (FsEntry),a
        ld hl,VfsName
        ld de,FsEntry+1
        call Vfs_CopyZ
        pop af
        jp nz,Vfs_Refuse               ; непустой каталог не удаляем
        jr .entry_found
.restore_failed:
        pop af
        jp Vfs_Refuse
.entry_found:
        ld hl,FsEntry
        call WC_DELETE
        jp z,Vfs_Refuse
        ld a,VFS_DELETE
        ld c,VFS_OK
        jp Vfs_Reply1

Vfs_Mkdir:
        ld a,VFS_MKDIR
        ld (VfsCmd),a
        ld hl,ProtoBuf
        call Vfs_TakePath
        jp c,Vfs_Refuse
        call Fs_ResetRoot
        call Vfs_SplitPath
        jp c,Vfs_Refuse
        jp z,Vfs_Refuse                 ; корень уже существует
        call Fs_SelectWork
        ld hl,VfsName                   ; API 73: только имя с нулём
        call WC_MKDIR
        jp nz,Vfs_Refuse
        ld a,VFS_MKDIR
        ld c,VFS_OK
        jp Vfs_Reply1

; Переименовать файл или каталог в пределах его текущего каталога.
; ESP заранее отделяет новый последний компонент от полного пути назначения,
; поэтому WC получает ровно свой ABI: HL=[атрибут][старое имя,0],
; DE=[новое имя,0]. Перемещение между каталогами этот API не выполняет.
Vfs_Rename:
        ld a,VFS_RENAME
        ld (VfsCmd),a
        ; Минимальное корректное тело: attr, старый ноль, один байт имени и
        ; новый ноль. Более короткий пакет нельзя разбирать как строки.
        ld hl,(ProtoRxLen)
        ld a,h
        or a
        jr nz,.length_ok
        ld a,l
        cp 4
        jp c,Vfs_Refuse
.length_ok:
        ld a,(ProtoBuf)
        ld (VfsAttr),a

        ; Сначала забираем вторую строку, пока ProtoBuf хранит сетевой UTF-8.
        call Vfs_TakeRenameName
        jp c,Vfs_Refuse

        ; Старый полный путь начинается после байта атрибута.
        ld hl,ProtoBuf+1
        call Vfs_TakePath
        jp c,Vfs_Refuse
        call Fs_ResetRoot
        call Vfs_SplitPath
        jp c,Vfs_Refuse
        jp z,Vfs_Refuse                 ; корень переименовывать нельзя

        ld a,(VfsAttr)
        ld (FsEntry),a
        ld hl,VfsName
        ld de,FsEntry+1
        call Vfs_CopyZ
        call Fs_SelectWork
        ld hl,FsEntry
        ld de,VfsName2
        call WC_RENAME
        jp z,Vfs_Refuse
        ld a,VFS_RENAME
        ld c,VFS_OK
        jp Vfs_Reply1

; Найти вторую нуль-терминированную строку тела RENAME, скопировать её в
; отдельный буфер, перевести UTF-8 в кодировку WC и проверить как одно имя.
Vfs_TakeRenameName:
        ld hl,ProtoBuf+1
        ld bc,(ProtoRxLen)
        dec bc                           ; байт атрибута уже пропущен
.skip_old:
        ld a,b
        or c
        jr z,.bad
        ld a,(hl)
        inc hl
        dec bc
        or a
        jr nz,.skip_old
        ; Сначала проверяем всю вторую строку в границах принятого payload.
        ; DE считает байты имени; старший байт обязан оставаться нулевым.
        push hl                          ; начало нового имени
        ld de,0
.measure_new:
        ld a,b
        or c
        jr z,.bad_pop
        ld a,(hl)
        inc hl
        dec bc
        or a
        inc de
        jr z,.measured
        ld a,d
        or a
        jr z,.measure_new
.bad_pop:
        pop hl
.bad:
        scf
        ret
.measured:
        ; После завершающего нуля хвоста быть не должно. Сам ноль не входит в
        ; длину имени, поэтому возвращаем счётчик на один байт назад.
        dec de
        ld a,b
        or c
        jr nz,.bad_pop
        ld a,d
        or a
        jr nz,.bad_pop
        pop hl
        ld de,VfsName2
        call Vfs_CopyZ
.convert:
        ld hl,VfsName2
        call Utf8_ToOemInPlace
        ret c
        ld hl,VfsName2
        jp Fs_ValidateName

; FILEX MOVE_RENAME принимает полные пути обеих сторон и выполняет также
; перенос между каталогами. Payload: [replace][attr]old,0,new,0.
Vfs_MoveRename:
        ld a,VFS_MOVE_RENAME
        ld (VfsCmd),a
        call Vfs_TakeMovePath2
        jp c,Vfs_Refuse
        ld a,(ProtoBuf)
        and FILEX_FLAG_REPLACE
        ld (VfsMoveFlags),a
        ld a,(ProtoBuf+1)
        ld (VfsAttr),a

        ; Разобрать источник и сохранить cluster его parent.
        ld hl,ProtoBuf+2
        call Vfs_TakePath
        jp c,Vfs_Refuse
        call Fs_ResetRoot
        call Vfs_SplitPath
        jp c,Vfs_Refuse
        jp z,Vfs_Refuse
        ld hl,CurrentDirectoryCluster
        ld de,VfsMoveSourceDir
        ld bc,4
        ldir
        ld a,(VfsAttr)
        ld hl,VfsName
        ld de,VfsMoveSource
        call Vfs_BuildMoveQuery
        ld (VfsMoveSourceLength),bc

        ; Второй путь уже скопирован до перезаписи VfsPath.
        ld hl,VfsPath2
        call Vfs_TakePath
        jp c,Vfs_Refuse
        call Fs_ResetRoot
        call Vfs_SplitPath
        jp c,Vfs_Refuse
        jp z,Vfs_Refuse
        ld hl,CurrentDirectoryCluster
        ld de,VfsMoveDestDir
        ld bc,4
        ldir
        ld a,(VfsAttr)
        ld hl,VfsName
        ld de,VfsMoveDest
        call Vfs_BuildMoveQuery
        ld (VfsMoveDestLength),bc

        call Vfs_FilexReset
        ld a,FILEX_OP_MOVE_RENAME
        ld (VfsFilexBlock+FILEX_P_OPERATION),a
        ld a,(VfsMoveFlags)
        ld (VfsFilexBlock+FILEX_P_FLAGS),a
        ld hl,VfsMoveSource
        ld (VfsFilexBlock+FILEX_P_BUFFER),hl
        ld hl,(VfsMoveSourceLength)
        ld (VfsFilexBlock+FILEX_P_LENGTH),hl
        ld hl,VfsMoveDest
        ld (VfsFilexBlock+FILEX_P_AUX),hl
        ld hl,(VfsMoveDestLength)
        ld (VfsFilexBlock+FILEX_P_AUX_LENGTH),hl
        ld hl,(VfsMoveSourceDir)
        ld (VfsFilexBlock+FILEX_P_SOURCE_DIR),hl
        ld hl,(VfsMoveSourceDir+2)
        ld (VfsFilexBlock+FILEX_P_SOURCE_DIR+2),hl
        ld hl,(VfsMoveDestDir)
        ld (VfsFilexBlock+FILEX_P_DEST_DIR),hl
        ld hl,(VfsMoveDestDir+2)
        ld (VfsFilexBlock+FILEX_P_DEST_DIR+2),hl
        ld hl,VfsFilexBlock
        call WC_FILEX
        ld c,a
        ld a,VFS_MOVE_RENAME
        jp Vfs_Reply1

; Проверить две строки в границах payload и сохранить второй путь.
Vfs_TakeMovePath2:
        ld hl,(ProtoRxLen)
        ld de,6
        or a
        sbc hl,de
        jr c,.bad
        ld hl,ProtoBuf+2
        ld bc,(ProtoRxLen)
        dec bc
        dec bc
.old:
        ld a,b
        or c
        jr z,.bad
        ld a,(hl)
        inc hl
        dec bc
        or a
        jr nz,.old
        push hl                         ; начало destination
        ld de,0
.new:
        ld a,b
        or c
        jr z,.bad_pop
        ld a,(hl)
        inc hl
        dec bc
        inc de
        or a
        jr z,.new_done
        ld a,d
        or a
        jr z,.new
.bad_pop:
        pop hl
.bad:
        scf
        ret
.new_done:
        ld a,b
        or c
        jr nz,.bad_pop                  ; после второго нуля хвоста быть не должно
        ld a,d
        or a
        jr nz,.bad_pop                  ; VfsPath2 принимает до 254 байт + zero
        pop hl
        ld de,VfsPath2
        call Vfs_CopyZ
        or a
        ret

; A=type, HL=name, DE=query. Возвращает BC=длина [type,name,0].
Vfs_BuildMoveQuery:
        ld (de),a
        inc de
        ld bc,1
.copy:
        ld a,(hl)
        ld (de),a
        inc bc
        or a
        ret z
        inc hl
        inc de
        jr .copy

; --- данные ------------------------------------------------------------------

VfsCmd:         db 0                    ; обрабатываемая команда
VfsAttr:        db 0                    ; атрибут для FENTRY
VfsMode:        db 0                    ; режим OPEN: 0=чтение, 1=замена, 2=append
VfsHasName:     db 0
VfsPath:        ds PATH_SIZE
VfsPath2:       ds PATH_SIZE            ; destination path для FILEX MOVE
VfsName:        ds PATH_SIZE
VfsName2:       ds PATH_SIZE            ; новое имя для RENAME
VfsMoveSource:  ds PATH_SIZE+1          ; [тип,имя,0]
VfsMoveDest:    ds PATH_SIZE+1
VfsMoveSourceLength:
                dw 0
VfsMoveDestLength:
                dw 0
VfsMoveSourceDir:
                ds 4
VfsMoveDestDir: ds 4
VfsMoveFlags:   db 0
VfsBuf:         ds IO_SIZE+16           ; тело ответа: статус + сектор либо имя
VfsFilexBlock:  ds FILEX_BLOCK_SIZE
VfsMetaBuffer:  ds FILEX_META_SIZE
VfsFilexCaps:   db 0
VfsFilexStatus: db 0
VfsFilexCount:  ds 4
VfsFilexStagePtr: ds 2
VfsFilexRemaining: ds 2
VfsFilexChunk:  ds 2
VfsFilexPart:   ds 2
VfsFilexWorkOffset: ds 4
VfsRandomOffset:
                ds 4
VfsWriteCount:  dw 0                    ; распаковано в page1:#C000 (0..#4000)
VfsWriteFailed: db 0                    ; APPEND уже отказал: вслепую не повторять
VfsExtendRemaining:
                ds 4                    ; сколько нулей осталось дописать
VfsExpectedSeq: db 0
VfsBlockActive: db 0
VfsBlockKind:   db 0
VfsBlockSeq:    db 0
VfsReplySeq:    db 0
VfsBlockRawLen: dw 0
VfsBlockStoredLen:
                dw 0
VfsBlockReceived:
                dw 0
VfsBlockNewReceived:
                dw 0
VfsBlockPart:   dw 0
VfsBlockCrc:    dw 0
VfsBlockNewCount:
                dw 0
VfsWindowActive:
                db 0
VfsWindowFlags: db 0
VfsWindowSeq:   db 0
VfsWindowTotal: dw 0
VfsWindowReceived:
                dw 0
VfsWindowNewReceived:
                dw 0
VfsWindowPart:  dw 0
VfsReadSeq:     db 0
VfsFatSeq:      db 0
VfsResponseSeq: db 0
VfsFatOffset:   ds 4
VfsFatMapRemaining:
                ds 2
VfsReadSectors: db 0
VfsReadFlags:   db 0
VfsReadFinal:   db 0
VfsReadWanted:  dw 0
VfsReadCount:   dw 0
VfsReadOffset:  dw 0
VfsReadNewOffset:
                dw 0
VfsReadPart:    dw 0
VfsReadPtr:     dw 0
VfsReadCrc:     dw 0
VfsOutStart:    dw 0
VfsLzSrcPtr:    dw 0
VfsLzSrcLeft:   dw 0
VfsLzOutPtr:    dw 0
VfsLzOutLeft:   dw 0
VfsLzProduced:  dw 0
VfsLzOffset:    dw 0
VfsLzLength:    dw 0
VfsLzToken:     db 0
                ALIGN 512
VfsCompBuf:     ds VFS_BLOCK_SIZE        ; максимум одного RAW/LZ4 блока
