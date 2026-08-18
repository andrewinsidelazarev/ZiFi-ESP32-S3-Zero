; Файловый слой плагина: работа с SD только через DOS API Wild Commander.
;
; WC для этого проекта — чёрный ящик: прямых обращений к портам SD здесь нет.
; Возможности записи ограничены самим API: создать файл нулевой длины и
; дописывать в конец (MKFILE + APPEND). Перезаписи середины файла нет, поэтому
; загрузка идёт строго последовательными блоками.
;
; Эти процедуры общие для сетевых серверов: протокол SMB разбирает ESP,
; а разбор путей и файловые операции остаются на стороне Z80.

PATH_SIZE   equ 256                     ; предел длины пути и имени
IO_SIZE     equ 512                     ; сектор FAT и размер одного LOAD512

Utf8_ToOemInPlace:
        push hl
        pop de
.next:
        ld a,(hl)
        inc hl
        or a
        jr z,.done
        cp #80
        jr c,.store
        cp #D0
        jr z,.lead_d0
        cp #D1
        jr z,.lead_d1
        jr .bad
.lead_d0:
        ld a,(hl)
        inc hl
        cp #81
        jr z,.upper_yo
        cp #90
        jr c,.bad
        cp #C0
        jr nc,.bad
        sub #10
        jr .store
.lead_d1:
        ld a,(hl)
        inc hl
        cp #91
        jr z,.lower_yo
        cp #80
        jr c,.bad
        cp #90
        jr nc,.bad
        add a,#60
        jr .store
.upper_yo:
        ld a,#F0
        jr .store
.lower_yo:
        ld a,#F1
.store:
        ld (de),a
        inc de
        jr .next
.done:
        ld (de),a
        or a
        ret
.bad:
        ld a,'?'
        ld (de),a
        inc de
        xor a
        ld (de),a
        scf
        ret

; Копировать внутреннюю строку CP866 из HL в сетевой UTF-8 по адресу DE без
; завершающего нуля. Неподдерживаемые служебные знаки CP866 заменяются на `?`.

Oem_CopyUtf8NoTerm:
.next:
        ld a,(hl)
        inc hl
        or a
        ret z
        cp #80
        jr c,.store
        cp #B0
        jr c,.d0_russian
        cp #E0
        jr c,.unsupported
        cp #F0
        jr c,.d1_russian
        cp #F0
        jr z,.upper_yo
        cp #F1
        jr z,.lower_yo
.unsupported:
        ld a,'?'
        jr .store
.d0_russian:
        ld c,a
        ld a,#D0
        ld (de),a
        inc de
        ld a,c
        add a,#10
        jr .store
.d1_russian:
        ld c,a
        ld a,#D1
        ld (de),a
        inc de
        ld a,c
        sub #60
        jr .store
.upper_yo:
        ld a,#D0
        ld (de),a
        inc de
        ld a,#81
        jr .store
.lower_yo:
        ld a,#D1
        ld (de),a
        inc de
        ld a,#91
.store:
        ld (de),a
        inc de
        jr .next

String_Equals:
.loop:
        ld a,(de)
        cp (hl)
        ret nz
        or a
        ret z
        inc de
        inc hl
        jr .loop

; Вычислить длину следующей порции файла. Вход: HL — байты, BC — длина.

File_Min256:
        push hl
        ld a,(FileRemaining+2)
        ld b,a
        ld a,(FileRemaining+3)
        or b
        jr nz,.full
        ld hl,(FileRemaining)
        ld a,h
        or a
        jr nz,.full
        ld a,l
        or a
        jr z,.zero
        ld c,l
        ld b,0
        jr .done
.full:
        ld bc,256
        jr .done
.zero:
        ld bc,0
.done:
        pop hl
        ret

File_SubBC:
        ld hl,(FileRemaining)
        or a
        sbc hl,bc
        ld (FileRemaining),hl
        ld hl,(FileRemaining+2)
        ld bc,0
        sbc hl,bc
        ld (FileRemaining+2),hl
        ret

; Загрузить файл потоково. ESP передаёт подтверждаемые фрагменты по 256 байт,
; VFS накапливает 512 байт перед APPEND, а заполненное TCP-кольцо создаёт штатное
; обратное давление. Замена не транзакционная: старый файл удаляется при OPEN.

Fs_SelectWork:
        ld d,0
        ld bc,#FFFF
        jp WC_STREAM

; Вернуть оба потока WC и текстовый CwdPath к корню выбранного SMB-ресурса.

Fs_ResetRoot:
        call Config_RootBothStreams
        ld hl,CwdPath
        ld (hl),'/'
        inc hl
        ld (hl),0
        xor a
        ld (CwdDepth),a
        ld hl,0
        ld (CurrentDirectoryCluster),hl
        ld (CurrentDirectoryCluster+2),hl
        ret

; HL — путь. Поддерживаются абсолютная и относительная формы, компоненты '.' и '..'.
; Абсолютный путь сначала сбрасывает поток в корень. Каждый компонент длиной не
; более 63 байтов проверяется через FAT. Переход `..` из корня остаётся в корне,
; что не позволяет SMB-клиенту выйти к другому устройству или панели WC.

Fs_ChangePath:
        ld a,(hl)
        or a
        jr z,.failed
        cp '/'
        jr nz,.component
        push hl
        call Fs_ResetRoot
        pop hl
.skip_slash:
        ld a,(hl)
        cp '/'
        jr nz,.component
        inc hl
        jr .skip_slash
.component:
        ld a,(hl)
        or a
        ret z
        ld de,PathComponent
        ld b,63
.copy:
        ld a,(hl)
        or a
        jr z,.component_done
        cp '/'
        jr z,.component_done
        ld (de),a
        inc de
        inc hl
        djnz .copy
        jr .failed
.component_done:
        xor a
        ld (de),a
        push hl
        ld hl,PathComponent
        ld de,PathDot
        call String_Equals
        jr z,.component_ok
        ld hl,PathComponent
        ld de,PathDotDot
        call String_Equals
        jr z,.parent
        ld hl,PathComponent
        call Fs_EnterDirectory
        jr c,.component_fail
        jr .component_ok
.parent:
        call Fs_ChangeToParent
        jr c,.component_fail
.component_ok:
        pop hl
        ld a,(hl)
        or a
        ret z
        inc hl
        jr .skip_slash
.component_fail:
        pop hl
.failed:
        scf
        ret

; Войти в один проверенный компонент каталога и синхронно обновить CwdPath.

Fs_EnterDirectory:
        call Path_CanAppend
        ret c
        push hl
        ld de,FsEntry+1
        ld a,#10
        ld (FsEntry),a
        call CopyZ
        call Fs_SelectWork
        ld hl,FsEntry
        call WC_FENTRY
        jr z,.failed_pop
        call Fs_CaptureDirectoryCluster
        call WC_GDIR
        pop hl
        call Path_Append
        ld a,(CwdDepth)
        inc a
        ld (CwdDepth),a
        or a
        ret
.failed_pop:
        pop hl
        scf
        ret

; Подняться на один FAT-каталог. При CwdDepth=0 операция успешна, но поток
; остаётся в корне общего ресурса — это безопасная обработка компонента `..`.

Fs_ChangeToParent:
        ld a,(CwdDepth)
        or a
        ret z                            ; корень является родителем самому себе
        call Fs_SelectWork
        ld hl,FsParentEntry
        call WC_FENTRY
        jr z,.failed
        call Fs_CaptureDirectoryCluster
        call WC_GDIR
        call Path_RemoveLast
        ld a,(CwdDepth)
        dec a
        ld (CwdDepth),a
        or a
        ret
.failed:
        scf
        ret

; Сохранить cluster текущей найденной directory ENTRY для FILEX MOVE_RENAME.
; FAT32 хранит младшее слово в +26, старшее (28 значащих бит) — в +20.
Fs_CaptureDirectoryCluster:
        ld de,FsRawEntry
        call WC_TENTRY
        ld hl,(FsRawEntry+26)
        ld (CurrentDirectoryCluster),hl
        ld hl,(FsRawEntry+20)
        ld a,h
        and #0F
        ld h,a
        ld (CurrentDirectoryCluster+2),hl
        ret

; Добавить компонент HL к CwdPath, вставив `/`, кроме случая самого корня.

Path_Append:
        push hl
        ld de,CwdPath
.end:
        ld a,(de)
        or a
        jr z,.at_end
        inc de
        jr .end
.at_end:
        ld a,e
        cp low (CwdPath+1)
        jr nz,.add_slash
        ld a,d
        cp high (CwdPath+1)
        jr z,.copy
.add_slash:
        ld a,'/'
        ld (de),a
        inc de
.copy:
        pop hl
        jp CopyZ

; Проверить, поместятся ли компонент, возможный разделитель и завершающий ноль
; в CwdPath. Входной указатель HL сохраняется. Выход: CF=1 — места недостаточно.

Path_CanAppend:
        push hl
        ld de,CwdPath
.find_end:
        ld a,(de)
        or a
        jr z,.at_end
        inc de
        jr .find_end
.at_end:
        ld a,e
        cp low (CwdPath+1)
        jr nz,.need_slash
        ld a,d
        cp high (CwdPath+1)
        jr z,.check_byte
.need_slash:
        inc de
.check_byte:
        ld a,d
        cp high (CwdPath+PATH_SIZE)
        jr c,.room
        jr nz,.overflow
        ld a,e
        cp low (CwdPath+PATH_SIZE)
        jr nc,.overflow
.room:
        ld a,(hl)
        inc hl
        inc de
        or a
        jr nz,.check_byte
        pop hl
        or a
        ret
.overflow:
        pop hl
        scf
        ret

; Удалить последний текстовый компонент CwdPath, сохранив `/` для корня.

Path_RemoveLast:
        ld hl,CwdPath
.find_end:
        ld a,(hl)
        or a
        jr z,.back
        inc hl
        jr .find_end
.back:
        dec hl
        ld a,l
        cp low CwdPath
        jr nz,.check
        ld a,h
        cp high CwdPath
        jr z,.root
.check:
        ld a,(hl)
        cp '/'
        jr z,.cut
        dec hl
        jr .back
.cut:
        ld a,l
        cp low CwdPath
        jr nz,.zero_here
        ld a,h
        cp high CwdPath
        jr nz,.zero_here
.root:
        ld hl,CwdPath+1
.zero_here:
        xor a
        ld (hl),a
        ret

; A — флаг записи; VfsName копируется в FsEntry.
; Выход: CF=1 — недопустимое имя.

Fs_MakeEntryFromArgument:
        ld (FsEntry),a
        ld hl,VfsName
        call Fs_ValidateName
        ret c
        ld hl,VfsName
        ld de,FsEntry+1
        call CopyZ
        or a
        ret

; Проверить имя для операций в текущем каталоге. Запрещены пустая строка, `.`/`..`,
; управляющие символы, `/`, обратная косая черта и `:`. Разделители путей здесь
; не допускаются намеренно: навигация выполняется отдельной командой CWD.

Fs_ValidateName:
        ld a,(hl)
        or a
        jr z,.bad
        ld de,PathDot
        push hl
        call String_Equals
        pop hl
        jr z,.bad
        ld de,PathDotDot
        push hl
        call String_Equals
        pop hl
        jr z,.bad
.loop:
        ld a,(hl)
        or a
        jr z,.ok
        cp 32
        jr c,.bad
        cp '/'
        jr z,.bad
        cp #5C
        jr z,.bad
        cp ':'
        jr z,.bad
        inc hl
        jr .loop
.ok:
        or a
        ret
.bad:
        scf
        ret

; ---------------------------------------------------------------------------
; Десятичные преобразования и работа с буферами
; ---------------------------------------------------------------------------

BufferLengthBC:
        ; Подсчитать длину, сохранив HL на начале отправляемого буфера.
        push hl
        ld bc,0
.loop:
        ld a,(hl)
        or a
        jr z,.done
        inc hl
        inc bc
        jr .loop
.done:
        pop hl
        ret

U16_ToDec:
        ld (NumberTemp),hl
        xor a
        ld (NumberTemp+2),a
        ld (NumberTemp+3),a
        jp U32Temp_ToDec

; HL указывает на 32-битное число от младшего байта; DE принимает ASCII
; и сдвигается за записанные символы.

U32_ToDec:
        push de
        ld de,NumberTemp
        ld bc,4
        ldir
        pop de

U32Temp_ToDec:
        xor a
        ld (NumberDigits),a
        ld hl,NumberTemp
        call U32_IsZero
        jr nz,.divide
        ld a,'0'
        ld (de),a
        inc de
        ret
.divide:
        call Div32By10
        push af
        ld a,(NumberDigits)
        inc a
        ld (NumberDigits),a
        ld hl,NumberTemp
        call U32_IsZero
        jr nz,.divide
.emit:
        pop af
        add a,'0'
        ld (de),a
        inc de
        ld a,(NumberDigits)
        dec a
        ld (NumberDigits),a
        jr nz,.emit
        ret

; Разделить NumberTemp на 10 и вернуть остаток в A.

Div32By10:
        push de
        ld ix,NumberTemp+3
        ld c,4
        xor a
.byte:
        ld h,a
        ld l,(ix+0)
        ld b,0
.subtract:
        ld a,h
        or a
        jr nz,.can_subtract
        ld a,l
        cp 10
        jr c,.byte_done
.can_subtract:
        ld de,10
        or a
        sbc hl,de
        inc b
        jr .subtract
.byte_done:
        ld (ix+0),b
        ld a,l
        dec ix
        dec c
        jr nz,.byte
        pop de
        ret

; HL указывает на четыре байта. Z, если значение равно нулю.

U32_IsZero:
        ld a,(hl)
        inc hl
        or (hl)
        inc hl
        or (hl)
        inc hl
        or (hl)
        ret

; ---------------------------------------------------------------------------
; Строки протокола и состояние
; ---------------------------------------------------------------------------


; --- данные файлового слоя ---------------------------------------------------

PathDot:         db ".",0
PathDotDot:      db "..",0
FsParentEntry:   db #10,"..",0

CwdDepth:        db 0                   ; глубина текущего каталога от корня
CurrentDirectoryCluster:
                ds 4                   ; cluster текущего parent, 0 = root
CwdPath:         ds PATH_SIZE           ; текущий путь в кодировке имён WC
SavedCwdPath:    ds PATH_SIZE
PathComponent:   ds 64                  ; один компонент пути при разборе
FsEntry:         ds PATH_SIZE+1         ; запись для FENTRY: [атрибут][имя,0]
FsRawEntry:      ds 32                  ; необработанная FAT ENTRY от API 51
MkFileBuffer:    ds PATH_SIZE+5         ; ABI MKFILE: [атрибут][размер LE32][имя,0]

FileSize:        ds 4                   ; размер открытого файла от WC
FileRemaining:   ds 4                   ; сколько осталось прочитать
DirEntryBuffer:  ds PATH_SIZE+9         ; запись каталога от FINDNEXT

NumberTemp:      ds 4                   ; рабочая память десятичного вывода
NumberDigits:    db 0
FileIoBuffer:    ds IO_SIZE             ; единственный секторный буфер плагина
