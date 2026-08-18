; Минимальный набор API Wild Commander v1.01 для плагина ZiFi FTP.
; Плагин вызывает общий диспетчер WC_API с номером функции в A. Ни один из этих
; переходников не обращается к портам SD: файловая система остаётся под контролем
; Wild Commander и его выбранного STREAM.

WC_API      equ #6006
WC_PAGE2    equ #6002
WC_PAGE3    equ #6003

FN_MNGC_PL  equ #00                    ; отобразить страницу плагина в #C000
FN_PRWOW    equ #01                    ; открыть окно и сохранить фон
FN_RRESB    equ #02                    ; восстановить сохранённую область экрана
FN_TXTPR    equ #0B                    ; вывести текст в окно
FN_GEDPL    equ #0F                    ; получить параметры текущего дисплея
FN_ESC      equ #17                    ; проверить клавишу Esc
FN_LOAD512  equ #30                    ; прочитать сектор открытого файла
FN_SAVE512  equ #31                    ; записать сектор открытого файла
FN_TENTRY   equ #33                    ; скопировать текущую FAT ENTRY (32 байта)
FN_STREAM   equ #39                    ; выбрать/создать файловый поток
FN_ADIR     equ #38                    ; начать перечисление каталога
FN_FINDNEXT equ #3A                    ; получить следующую запись каталога
FN_FENTRY   equ #3B                    ; найти FAT-запись файла или каталога
FN_GFILE    equ #3E                    ; открыть найденный файл
FN_GDIR     equ #3F                    ; войти в найденный каталог
FN_MKFILE   equ #48                    ; создать файл заданного размера
FN_MKDIR    equ #49                    ; создать каталог
FN_RENAME   equ #4A                    ; переименовать FAT-запись
FN_DELETE   equ #4B                    ; удалить файл или пустой каталог
FN_APPEND   equ #4C                    ; дописать 0..#4000 байт в конец найденного файла
FN_FILEX    equ #4D                    ; позиционный файловый API, 32-байтовый блок
FN_MNG8_PL  equ #4F                    ; отобразить страницу плагина в #8000
FN_INT_PL   equ #56                    ; режим взаимодействия плагина с интерфейсом WC

; Специальные селекторы STREAM: CLONE копирует состояние активной панели только
; при старте, ROOT переводит уже созданный поток в корень его собственного тома.
STREAM_CLONE equ #FE
STREAM_ROOT  equ #FF

; Публичный FILEX ABI v1 (Wild Commander Improved API 77).
FILEX_API_VERSION       equ 1
FILEX_BLOCK_SIZE        equ 32
FILEX_P_OPERATION       equ 2
FILEX_P_FLAGS           equ 3
FILEX_P_OFFSET          equ 4
FILEX_P_BUFFER          equ 8
FILEX_P_LENGTH          equ 10
FILEX_P_AUX             equ 12
FILEX_P_AUX_LENGTH      equ 14
FILEX_P_SOURCE_DIR      equ 16
FILEX_P_DEST_DIR        equ 20
FILEX_P_RESULT_COUNT    equ 24
FILEX_P_STATUS          equ 28
FILEX_P_RESULT_FLAGS    equ 29

FILEX_OP_QUERY_CAPS     equ 0
FILEX_OP_READ_AT        equ 1
FILEX_OP_WRITE_AT       equ 2
FILEX_OP_SET_EOF32      equ 3
FILEX_OP_GET_FS_INFO    equ 4
FILEX_OP_MOVE_RENAME    equ 5
FILEX_OP_SET_METADATA   equ 6
FILEX_OP_READ_FAT       equ 7

FILEX_STATUS_OK         equ #00
FILEX_STATUS_EOF        equ #01
FILEX_STATUS_NO_CONTEXT equ #15
FILEX_STATUS_COMMITTED_CLEANUP equ #25
FILEX_STATUS_INTERNAL   equ #26
FILEX_FLAG_REPLACE      equ 1
FILEX_FLAG_REFRESH_FREE equ 1
FILEX_FS_SIZE           equ 48
FILEX_META_SIZE         equ 16
