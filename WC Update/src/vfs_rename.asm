; Замена файла копией для обновлятора: VFS MOVE_RENAME (#5D) и RENAME (#59)
; поверх файловых модулей FTP-плагина.
;
; Обновлятор ставит проверенную копию WCUPD.TMP на место файла так:
; - основной путь — MOVE_RENAME с REPLACE: FILEX Wild Commander Improved
;   перенаправляет запись файла на цепочку копии одной записью сектора и при
;   сбое возвращает прежнюю, так что имя файла не пропадает ни на миг;
; - если у WC нет FILEX MOVE, плагин отвечает #FE, ничего не трогая, и ESP
;   удаляет старый файл и переименовывает копию через RENAME (API 74).

VFS_RENAME      equ #59
VFS_MOVE_RENAME equ #5D
VFS_MOVE_UNSUPPORTED equ #FE            ; у WC нет FILEX MOVE: ничего не менялось

; MOVE_RENAME: [флаги: бит 0 — REPLACE][атрибут][старый путь,0][новый путь,0],
; пути полные. Ответ — статус FILEX как есть (0 — готово; #25 — готово, но
; старая цепочка не освобождена; #10..#1D — отказ до изменений; #20..#24 —
; сбой носителя), #FE — нет FILEX MOVE, 1 — путь не разобран.
;
; FILEX принимает каталоги кластерами: путь каталога проходится от корня
; заново (Vfs_DirCluster), кластер компонента снимает TENTRY после FENTRY.
Vfs_MoveRename:
        ld a,VFS_MOVE_RENAME
        ld (VfsCmd),a
        call Vfs_FilexCapsOnce
        ld a,(VfsFilexCaps)
        and FILEX_CAP_MOVE_RENAME
        ld c,VFS_MOVE_UNSUPPORTED
        jp z,.reply
        call Vfs_TakeMovePath2          ; второй путь — в VfsName2
        jp c,Vfs_Refuse
        ld a,(ProtoBuf)
        and FILEX_FLAG_REPLACE
        ld (VfsMoveFlags),a
        ld a,(ProtoBuf+1)
        ld (VfsAttr),a

        ld hl,ProtoBuf+2
        ld de,VfsMoveSource
        call Vfs_MoveSide
        jp c,Vfs_Refuse
        ld (VfsMoveSourceLength),bc
        ld hl,VfsDirCluster
        ld de,VfsMoveSourceDir
        ld bc,4
        ldir
        ; Запрос назначения — в FsEntry: проход каталога его больше не займёт.
        ld hl,VfsName2
        ld de,FsEntry
        call Vfs_MoveSide
        jp c,Vfs_Refuse
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
        ld hl,FsEntry
        ld (VfsFilexBlock+FILEX_P_AUX),hl
        ld hl,(VfsMoveDestLength)
        ld (VfsFilexBlock+FILEX_P_AUX_LENGTH),hl
        ld hl,VfsMoveSourceDir
        ld de,VfsFilexBlock+FILEX_P_SOURCE_DIR
        ld bc,4
        ldir
        ld hl,VfsDirCluster
        ld de,VfsFilexBlock+FILEX_P_DEST_DIR
        ld bc,4
        ldir
        ld a,1
        ld (VfsChanged),a               ; запись могла лечь и при ответе-ошибке
        ld hl,VfsFilexBlock
        call WC_FILEX
        ld c,a                          ; статус FILEX
.reply:
        ld a,VFS_MOVE_RENAME
        jp Vfs_Reply1

; HL — полный путь (UTF-8), DE — буфер запроса [атрибут,имя,0].
; Выход: CF=0, BC — длина запроса, VfsDirCluster — кластер каталога пути;
; CF=1 — путь не разобран, это корень или каталога нет.
Vfs_MoveSide:
        push de
        call Vfs_TakePath
        jr c,.bad
        ld a,(VfsPath)
        cp '/'
        jr nz,.bad                      ; только полный путь: имя <= 254 байт
        call Fs_ResetRoot
        call Vfs_SplitPath              ; VfsName — имя, VfsPath — каталог
        jr c,.bad
        jr z,.bad
        call Vfs_DirCluster
        jr c,.bad
        pop de
        ld a,(VfsAttr)
        ld hl,VfsName
        jr Vfs_BuildMoveQuery
.bad:
        pop de
        scf
        ret

; A — атрибут, HL — имя, DE — запрос. Выход: BC — длина [атрибут,имя,0], CF=0.
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

; Кластер каталога VfsPath ("/", "/WC", "/WC/MENU"): путь проходится от корня,
; после каждого FENTRY кластер берётся из самой записи (TENTRY): младшее
; слово — +26, старшее (28 значащих бит FAT32) — +20.
; Выход: CF=0 и VfsDirCluster (0 — корень); CF=1 — каталога нет.
Vfs_DirCluster:
        call Fs_ResetRoot
        ld hl,0
        ld (VfsDirCluster),hl
        ld (VfsDirCluster+2),hl
        ld hl,VfsPath
.slash:
        ld a,(hl)
        cp '/'
        jr nz,.part
        inc hl
        jr .slash
.part:
        or a
        ret z                           ; путь кончился: CF=0
        ld a,#10
        ld (FsEntry),a
        ld de,FsEntry+1
.copy:
        ld a,(hl)
        or a
        jr z,.named
        cp '/'
        jr z,.named
        ld (de),a
        inc hl
        inc de
        jr .copy
.named:
        xor a
        ld (de),a
        push hl
        call Fs_SelectWork
        ld hl,FsEntry
        call WC_FENTRY
        jr z,.missing
        ld de,VfsRawEntry
        call WC_TENTRY
        ld hl,(VfsRawEntry+26)
        ld (VfsDirCluster),hl
        ld hl,(VfsRawEntry+20)
        ld a,h
        and #0F
        ld h,a
        ld (VfsDirCluster+2),hl
        call WC_GDIR
        pop hl
        jr .slash
.missing:
        pop hl
        scf
        ret

; Проверить обе строки MOVE в границах принятого payload и скопировать
; второй путь в VfsName2 (первый разберёт Vfs_TakePath из ProtoBuf).
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
        push hl                         ; начало второго пути
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
        jr nz,.bad_pop                  ; до 254 байт и ноль
        pop hl
        ld de,VfsName2
        call Vfs_CopyZ
        or a
        ret

; RENAME: [атрибут][старый путь,0][новое имя,0] -> [статус]. Запасной путь
; обновлятора для WC без FILEX MOVE. HL=[атрибут][старое имя,0],
; DE=[новое имя,0] — ровно ABI WC; перенос между каталогами API 74 не делает.
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
        jr z,.refused
        ld a,1
        ld (VfsChanged),a               ; каталог изменился: панели WC перечитать
        ld a,VFS_RENAME
        ld c,VFS_OK
        jp Vfs_Reply1
.refused:
        ; Код WC уходит в ESP как есть: #FF — не удался и откат, на цепочке
        ; могут остаться две записи, копию удалять нельзя; прочие — отказ
        ; без изменений. Ноль при Z — всё равно отказ.
        or a
        jr nz,.code
        inc a
.code:
        ld c,a
        inc a
        jr nz,.reply
        ld a,1
        ld (VfsChanged),a               ; #FF: каталог мог измениться
.reply:
        ld a,VFS_RENAME
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
        ld hl,VfsName2
        call Utf8_ToOemInPlace
        ret c
        ld hl,VfsName2
        jp Fs_ValidateName

; Буферы RENAME/MOVE лежат на FileIoBuffer из fs.asm: секторный буфер
; FTP-модулей больше никем не используется, а страница кода заполнена.
VfsName2        equ FileIoBuffer        ; новое имя RENAME / второй путь MOVE
VfsMoveSource   equ FileIoBuffer+PATH_SIZE ; [тип,имя,0] источника MOVE
        ASSERT 2*PATH_SIZE <= IO_SIZE
VfsRawEntry:    ds 32                   ; запись каталога из TENTRY
VfsMoveSourceDir: ds 4
VfsDirCluster:  ds 4
VfsMoveSourceLength: dw 0
VfsMoveDestLength: dw 0
VfsMoveFlags:   db 0
