; Самостоятельный FAT32: рабочая страница #0F, код #0E, данные в окне #C000.
; Повторный вход сохраняет исходное отображение; выход сохраняет AF результата.
sd_init:
        CALL init_sd_card
        LD HL,#3900
        XOR A
        CALL FAT32_BIND
        CALL FAT32_DEVICE_INIT
        JP NZ,ER0
        CALL FAT32_MOUNT
        JP NZ,ER1
        JP sd_exit

init_sd_card:
        LD A,(fat_active)
        OR A
        RET NZ
        INC A
        LD (fat_active),A
        LD A,(restore_page0+1)
        LD (fat_page0),A
        LD A,(restore_page1+1)
        LD (fat_page1),A
        LD A,(restore_page3+1)
        LD (fat_page3),A
        LD A,(save_mode+1)
        LD (fat_save_mode),A
        CALL off_int_dma
        LD A,sd_driver_page
        CALL set_page0
        LD A,fat32_code_page
        JP set_page1
sd_exit:
        PUSH AF
        LD A,(fat_active)
        OR A
        JR Z,.done
        LD A,(fat_page0)
        CALL set_page0
        LD A,(fat_page1)
        CALL set_page1
        LD A,(fat_page3)
        CALL set_page3
        LD A,(fat_save_mode)
        CALL sw_int_dma
        ; Автопереход музыки разрешён только после восстановления всех банков.
        XOR A
        LD (fat_active),A
.done:
        POP AF
        RET

save_downloaded_file:
        CALL init_sd_card
        LD DE,disk_icon
        CALL set_icon
        LD HL,(read_threads+thread.full_len)
        LD (fat_remaining),HL
        LD A,(read_threads+thread.full_len_high)
        LD (fat_remaining+2),A
        LD HL,(read_threads+thread.adress)
        ; thread.adress — адрес в окне #4000, thread.page уже задаёт банк.
        LD A,H
        SUB HIGH get_buffer
        JR C,.bad_range
        CP #40
        JR NC,.bad_range
        LD H,A
        LD A,(read_threads+thread.page)
        LD B,download_page+#28
        CP download_page
        JR NC,.page_limit
        CP music_page
        JR C,.bad_range
        LD B,music_page+2
.page_limit:
        CP B
        JR NC,.bad_range
        LD (fat_data_page),A
        LD A,B
        LD (.end_page+1),A
        LD (fat_offset),HL
        ; Проверяем весь диапазон до создания файла: конец <= страницы #48.
        LD DE,(fat_remaining)
        ADD HL,DE
        LD A,(fat_remaining+2)
        ADC A,0
        JR C,.bad_range
        CP #0B
        JR NC,.bad_range
        LD D,A
        LD A,H:RLCA:RLCA:AND 3
        LD E,A
        LD A,D:ADD A,A:ADD A,A:ADD A,E
        LD E,A
        LD A,(fat_data_page)
        ADD A,E
.end_page:
        CP download_page+#28
        JR C,.create
        JR NZ,.bad_range
        LD A,H:AND #3F:OR L
        JR Z,.create
.bad_range:
        LD A,FAT32_BAD_BUFFER
        JP fat_save_failed
.create:
        ; APPEND фиксирует точное число принятых байтов, без округления файла.
        LD HL,0
        LD (FILE+1),HL
        LD (FILE+3),HL          ; заодно нулевой флаг в FILE+4 для FIND/DELETE
        CALL fat_prepare_target
        JP NZ,fat_save_failed
.loop:
        LD HL,(fat_remaining)
        LD A,(fat_remaining+2)
        OR H:OR L
        JR Z,.close
        LD A,(fat_data_page)
        CALL set_page3
        LD HL,#4000,DE,(fat_offset)
        OR A:SBC HL,DE
        LD A,(fat_remaining+2)
        OR A
        JR NZ,.full
        LD DE,(fat_remaining)
        PUSH HL
        OR A:SBC HL,DE
        POP HL
        JR C,.full
        LD B,D,C,E
        JR .chunk
.full:
        LD B,H,C,L
.chunk:
        LD (fat_chunk),BC
        LD HL,(fat_offset)
        LD A,H:OR #C0:LD H,A
        CALL FAT32_APPEND
        JR NZ,fat_save_failed
        LD HL,(fat_remaining),BC,(fat_chunk)
        OR A:SBC HL,BC
        LD (fat_remaining),HL
        LD A,(fat_remaining+2)
        SBC A,0
        LD (fat_remaining+2),A
        LD HL,(fat_offset)
        ADD HL,BC
        BIT 6,H
        JR Z,.offset
        LD HL,0
        LD A,(fat_data_page)
        INC A
        LD (fat_data_page),A
.offset:
        LD (fat_offset),HL
        JR .loop
.close:
        CALL FAT32_CLOSE
        JR NZ,fat_save_failed
        CALL fat_commit_replace
        JP NZ,fat_save_failed
        LD HL,status_copy
        CALL set_ports
        XOR A
        JP sd_exit
fat_save_failed:
        LD (fat_last_error),A
        CALL sd_exit
        LD HL,fat_save_error_msg
        CALL win_error_call     ; окно, как у ошибок загрузки
        LD A,(fat_last_error)
        OR A
        SCF
        RET
fat_save_error_msg:
        DB "SD save failed; file may be partial",0,0

set_download_dir:
        CALL init_sd_card
        CALL FAT32_SET_ROOT
        JR C,.failed
        LD HL,DIR_zifi
        CALL fat_ensure_dir
        JR NZ,.failed
        LD HL,DIR_download
        CALL fat_ensure_dir
        JR NZ,.failed
        LD HL,DIR_date
        CALL fat_ensure_dir
        JP Z,sd_exit
.failed:
        JP ER3
fat_ensure_dir:
        PUSH HL
        CALL FAT32_FIND
        POP HL
        RET C
        JR NZ,.enter
        PUSH HL
        INC HL
        CALL FAT32_MKDIR
        POP HL
        RET NZ
        CALL FAT32_FIND
        RET C
        JR NZ,.enter
        LD A,FAT32_NO_FILE
        OR A
        RET
.enter:
        JP FAT32_SET_DIR

fat_active: DB 0
fat_page0: DB 0
fat_page1: DB 0
fat_page3: DB 0
fat_save_mode: DB 0
fat_data_page: DB 0
fat_offset: DW 0
fat_remaining: DS 3
fat_chunk: DW 0
fat_last_error: DB 0
