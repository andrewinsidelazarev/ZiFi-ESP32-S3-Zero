# Фактический SMB-протокол: Windows ↔ Linux Samba

Сформировано: `2026-08-26T15:00:06.504155+00:00`

Клиент: Windows `192.168.1.50`; сервер: Fedora/Samba `192.168.1.116`.
Эталон: Samba 4.24.5, SMB 3.1.1. Захват выполнен на Linux-интерфейсе `wlp3s0`.

## Границы доказательства

Это фактический сетевой обмен штатного Windows SMB redirector с Samba для 33 размеченных операций.
SMB encryption не применялось, поэтому заголовки, имена, структуры и payload доступны в PCAPNG.
UDP-маркеры `SMBTEST|Oxx|START/END|...` однозначно связывают сетевые кадры со строками таблицы.
В таблице последовательности сжаты, но точные кадры сохранены в `smb2_frames.csv`, полный разбор — в
`smb2_full_decode.txt` и `smb2_packets.json`.

## Артефакты и целостность

- Канонический PCAPNG: `windows_samba_reference_canonical_20260826.pcapng`; SHA-256 `28C12EDDA83029E773B7551C809C56220079A60780FAF74594EEC430040E518D`.
- Журнал 33 операций: `windows_operations_canonical.jsonl`; SHA-256 `A5461FC36B019D09FB25EA2ECC6B84A86BE45D2FD9D624EDD0DE5F5B6673CB58`.
- SMB2-кадров: `511`; UDP-маркеров: `66`.
- tcpdump: 0 packets dropped by kernel (см. `../logs/tcpdump_reference_canonical.log`).

## Ключевые свойства сеанса

- Диалект: SMB `3.1.1`; Windows сообщил `Signed=False`, `Encrypted=False`.
- CREATE/negotiate context tags в трассе: `MxAc, RqLs, DH2Q, QFid, AlSi`.
- CreditCharge: min `0`, max `2`; credits requested: min `0`, max `33`; granted: min `0`, max `33`.
- Compound/related SMB2 frames: `39`; signed frames: `5`.
- Не наблюдались в этом workload: `LOGOFF, TREE_DISCONNECT, LOCK, ECHO`. Их отсутствие — тоже фактическое поведение Windows; нельзя дорисовывать их в ZiFi только ради симметрии.

## SMB2-команды, реально встреченные в потоке

| Opcode | Команда | Запросов | Ответов | Фактические статусы | Назначение |
|---:|---|---:|---:|---|---|
| 0 | `NEGOTIATE` | 1 | 1 | 0x00000000 STATUS_SUCCESS×1 | согласование диалекта, возможностей, signing/encryption и negotiate contexts |
| 1 | `SESSION_SETUP` | 2 | 2 | 0xc0000016 STATUS_MORE_PROCESSING_REQUIRED×1, 0x00000000 STATUS_SUCCESS×1 | аутентификация и создание SMB-сеанса |
| 3 | `TREE_CONNECT` | 1 | 1 | 0x00000000 STATUS_SUCCESS×1 | подключение к ресурсу (tree/share) |
| 5 | `CREATE` | 71 | 71 | 0x00000000 STATUS_SUCCESS×58, 0xc0000034 STATUS_OBJECT_NAME_NOT_FOUND×8, 0xc000003a STATUS_OBJECT_PATH_NOT_FOUND×4, 0xc0000033 STATUS_OBJECT_NAME_INVALID×1 | создание или открытие файла/каталога; leases/oplocks и CREATE contexts |
| 6 | `CLOSE` | 56 | 56 | 0x00000000 STATUS_SUCCESS×56 | закрытие FileId, иногда с финальными метаданными |
| 7 | `FLUSH` | 3 | 5 | 0x00000000 STATUS_SUCCESS×3, 0x00000103 STATUS_PENDING×2 | принудительный сброс данных на серверное хранилище |
| 8 | `READ` | 10 | 10 | 0x00000000 STATUS_SUCCESS×10 | чтение диапазона файла |
| 9 | `WRITE` | 7 | 8 | 0x00000000 STATUS_SUCCESS×7, 0x00000103 STATUS_PENDING×1 | запись диапазона файла |
| 11 | `IOCTL` | 16 | 16 | 0xc0000010 STATUS_INVALID_DEVICE_REQUEST×11, 0x00000000 STATUS_SUCCESS×5 | FSCTL/IOCTL: возможности, offload/copy, resiliency и служебные запросы |
| 12 | `CANCEL` | 1 | 0 | — | отмена ранее ожидающего асинхронного запроса |
| 14 | `QUERY_DIRECTORY` | 4 | 4 | 0x00000000 STATUS_SUCCESS×2, 0x80000006 STATUS_NO_MORE_FILES×2 | перечисление каталога по шаблону |
| 15 | `CHANGE_NOTIFY` | 2 | 4 | 0x00000103 STATUS_PENDING×2, 0x00000000 STATUS_SUCCESS×1, 0xc0000120 STATUS_CANCELLED×1 | асинхронное уведомление об изменениях каталога |
| 16 | `QUERY_INFO` | 68 | 68 | 0x00000000 STATUS_SUCCESS×57, 0x80000005 STATUS_BUFFER_OVERFLOW×8, 0xc0000034 STATUS_OBJECT_NAME_NOT_FOUND×3 | чтение информации о файле, ФС, security descriptor или потоке |
| 17 | `SET_INFO` | 32 | 32 | 0x00000000 STATUS_SUCCESS×31, 0xc0000101 STATUS_DIRECTORY_NOT_EMPTY×1 | изменение EOF, времени, атрибутов, имени, disposition или ACL |
| 18 | `OPLOCK_BREAK` | 0 | 2 | 0x00000000 STATUS_SUCCESS×2 | разрыв/подтверждение lease или oplock |

## Полная таблица по файловым операциям

| № | ID | Операция Windows | Результат | SMB2-запросы по порядку | Статусы ответов | Ключевые фактические поля | Кадры |
|---:|---|---|---|---|---|---|---|
| 1 | `O00_CONNECT` | Подключение Windows к шару: negotiate, authentication, tree connect | `OK`, 1625.845 ms | NEGOTIATE → SESSION_SETUP×2 → TREE_CONNECT → IOCTL | 0x00000000 STATUS_SUCCESS; 0xc0000016 STATUS_MORE_PROCESSING_REQUIRED | ioctl=0x001401fc; result={"ServerName":"192.168.1.116","ShareName":"andrew","UserName":"WIN-SMUHQMJHFPG\\Администратор","Dialect":"3.1.1","Signed":false,"Encrypted":false,"NumOpens":2} | 5–15 (10 SMB2 frames) |
| 2 | `O01_QUERY_TEST_ROOT` | Открытие и перечисление существующего тестового каталога | `OK`, 125.019 ms | CREATE → QUERY_DIRECTORY×2 | 0x00000000 STATUS_SUCCESS; 0x80000006 STATUS_NO_MORE_FILES | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826, ., .., reports …; disp=OPEN; contexts=MxAc/RqLs; result=entries=4 | 19–24 (4 SMB2 frames) |
| 3 | `O02_CREATE_DIRECTORY` | Создание рабочего каталога | `OK`, 15.63 ms | CREATE | 0x00000000 STATUS_SUCCESS | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows; disp=CREATE; contexts=DH2Q/MxAc/QFid/RqLs | 28–29 (2 SMB2 frames) |
| 4 | `O03_CREATE_EMPTY_FILE` | Создание и закрытие пустого файла | `OK`, 31.208 ms | CREATE | 0x00000000 STATUS_SUCCESS | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//primary.bin; disp=CREATE; contexts=DH2Q/MxAc/QFid/RqLs | 33–34 (2 SMB2 frames) |
| 5 | `O04_WRITE_AND_FLUSH` | Перезапись, запись 65 537 байт, flush до носителя и close | `OK`, 281.136 ms | CLOSE → CREATE → SET_INFO → WRITE → FLUSH → QUERY_INFO → CLOSE | 0x00000000 STATUS_SUCCESS; 0x00000103 STATUS_PENDING | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//primary.bin; write=65537 B; offset=0; disp=OVERWRITE_IF; class=0x01; contexts=DH2Q/MxAc/RqLs; result=bytes=65537 | 38–73 (15 SMB2 frames) |
| 6 | `O05_QUERY_METADATA` | Чтение размера, атрибутов и времён файла | `OK`, 15.571 ms | — | — | —; result={"length":65537,"attributes":"Archive","creation_utc":"2026-08-26T14:49:51.0416780Z","write_utc":"2026-08-26T14:49:51.5472937Z"} | — |
| 7 | `O06_READ_AND_HASH` | Полное чтение файла и вычисление SHA-256 | `OK`, 140.554 ms | CREATE → READ×2 → QUERY_INFO → READ | 0x00000000 STATUS_SUCCESS | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//primary.bin; read=32768/28673/4096 B; offset=0/36864/32768; disp=OPEN; class=0x01; contexts=DH2Q/MxAc/QFid/RqLs; result=bytes=65537 sha256=718B9A71949FC252663F30DC7B02BE55A5372E6CFB091FD78A86D5868A995E21 | 86–159 (10 SMB2 frames) |
| 8 | `O07_APPEND` | Дописывание 4 097 байт в конец | `OK`, 46.949 ms | CLOSE → CREATE → WRITE → QUERY_INFO → CLOSE | 0x00000000 STATUS_SUCCESS | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//primary.bin; write=4097 B; offset=65537; disp=OPEN_IF; class=0x01; contexts=DH2Q/MxAc/RqLs; result=bytes=4097 | 164–175 (10 SMB2 frames) |
| 9 | `O08_RANDOM_WRITE` | Seek и запись 64 байт по смещению 32 760 | `OK`, 515.551 ms | CREATE → QUERY_INFO×2 → SET_INFO → READ×2 → QUERY_INFO → SET_INFO → QUERY_INFO → READ → QUERY_INFO → WRITE | 0x00000000 STATUS_SUCCESS | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//primary.bin; write=8192 B; read=32768/28674/8192 B; offset=0/40960/32768/28672; disp=OPEN; class=0x02/0x01; contexts=DH2Q/MxAc/QFid/RqLs | 186–316 (22 SMB2 frames) |
| 10 | `O09_LOCK_UNLOCK` | Установка и снятие байтовой блокировки | `OK`, 46.851 ms | QUERY_INFO → SET_INFO → QUERY_INFO → SET_INFO → QUERY_INFO | 0x00000000 STATUS_SUCCESS | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//primary.bin; class=0x01 | 329–338 (10 SMB2 frames) |
| 11 | `O10_TRUNCATE` | Усечение файла до 12 345 байт | `OK`, 46.808 ms | QUERY_INFO×2 → SET_INFO×2 → WRITE → FLUSH → QUERY_INFO → IOCTL | 0x00000000 STATUS_SUCCESS; 0x00000103 STATUS_PENDING; 0xc0000010 STATUS_INVALID_DEVICE_REQUEST | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//primary.bin; write=57 B; offset=12288/0; class=0x01; ioctl=0x00090284 | 342–359 (17 SMB2 frames) |
| 12 | `O11_EXTEND` | Расширение файла до 70 000 байт | `OK`, 62.442 ms | QUERY_INFO → SET_INFO → QUERY_INFO → SET_INFO → QUERY_INFO → SET_INFO×2 → FLUSH → QUERY_INFO → IOCTL | 0x00000000 STATUS_SUCCESS; 0x00000103 STATUS_PENDING; 0xc0000010 STATUS_INVALID_DEVICE_REQUEST | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//primary.bin; offset=0; class=0x01; ioctl=0x00090284 | 363–385 (21 SMB2 frames) |
| 13 | `O12_SET_TIMES` | Изменение creation/access/write timestamps | `OK`, 78.168 ms | CREATE → QUERY_INFO×2 → SET_INFO → QUERY_INFO → IOCTL → QUERY_INFO×2 → SET_INFO → QUERY_INFO → IOCTL → QUERY_INFO×2 → SET_INFO → QUERY_INFO → IOCTL | 0x00000000 STATUS_SUCCESS; 0xc0000010 STATUS_INVALID_DEVICE_REQUEST | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//primary.bin; offset=0; disp=OPEN; class=0x01; ioctl=0x00090284; contexts=DH2Q/MxAc/RqLs | 394–425 (32 SMB2 frames) |
| 14 | `O13_SET_ATTRIBUTES` | Установка и снятие DOS-атрибута Hidden | `OK`, 46.81 ms | CREATE → SET_INFO → QUERY_INFO → CLOSE → IOCTL → CLOSE → CREATE → SET_INFO → QUERY_INFO → IOCTL → CLOSE | 0x00000000 STATUS_SUCCESS; 0xc0000010 STATUS_INVALID_DEVICE_REQUEST | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//primary.bin, Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows; offset=0; disp=OPEN; class=0x01; ioctl=0x00090284; contexts=MxAc | 429–449 (19 SMB2 frames) |
| 15 | `O14_ENUMERATE_PATTERN` | Перечисление каталога по шаблону *.bin | `OK`, 0 ms | CREATE → QUERY_DIRECTORY×2 | 0x00000000 STATUS_SUCCESS; 0x80000006 STATUS_NO_MORE_FILES | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows, ., .., primary.bin; disp=OPEN; contexts=MxAc/RqLs; result=matches=1 | 453–456 (4 SMB2 frames) |
| 16 | `O15_RENAME_FILE` | Переименование файла | `OK`, 31.249 ms | CREATE → IOCTL → CLOSE×3 → CREATE → SET_INFO → QUERY_INFO → CREATE → CLOSE → IOCTL → CLOSE | 0x00000000 STATUS_SUCCESS; 0xc0000010 STATUS_INVALID_DEVICE_REQUEST | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//primary.bin, Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//renamed.bin, Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows; offset=0; disp=OPEN; class=0x01; ioctl=0x00090284; contexts=MxAc/DH2Q/RqLs | 460–481 (22 SMB2 frames) |
| 17 | `O16_COPY_FILE` | Копирование файла внутри одной шары | `OK`, 125.686 ms | CREATE → READ×2 → QUERY_INFO×4 → CREATE → IOCTL → SET_INFO → IOCTL → SET_INFO → QUERY_INFO → IOCTL → CLOSE×2 | 0x00000000 STATUS_SUCCESS; 0xc0000010 STATUS_INVALID_DEVICE_REQUEST | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//renamed.bin, Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//copy.bin; read=32768/29040 B; offset=0/40960; disp=OPEN/CREATE; class=0x01/0x03; ioctl=0x00140078/0x001440f2/0x00090284; contexts=DH2Q/MxAc/QFid/RqLs/AlSi; result=bytes=70000 | 485–560 (28 SMB2 frames) |
| 18 | `O17_OVERWRITE_COPY` | Копирование с заменой существующего назначения | `OK`, 124.94 ms | CREATE → WRITE → QUERY_INFO → CLOSE → CREATE → READ×2 → QUERY_INFO×4 → CREATE → IOCTL → SET_INFO → IOCTL → SET_INFO → QUERY_INFO → IOCTL → CLOSE×2 | 0x00000000 STATUS_SUCCESS; 0xc0000010 STATUS_INVALID_DEVICE_REQUEST | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//copy.bin, Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//renamed.bin; write=15 B; read=32768/29040 B; offset=0/40960; disp=OVERWRITE_IF/OPEN; class=0x01/0x03; ioctl=0x00140078/0x001440f2/0x00090284; contexts=DH2Q/MxAc/QFid/RqLs/AlSi; result=bytes=70000 | 564–653 (36 SMB2 frames) |
| 19 | `O18_ALTERNATE_DATA_STREAM` | Попытка создать/прочитать/удалить alternate data stream | `OK`, 62.418 ms | CREATE×2 → QUERY_INFO×2 → CLOSE | 0xc0000033 STATUS_OBJECT_NAME_INVALID; 0x00000000 STATUS_SUCCESS; 0x80000005 STATUS_BUFFER_OVERFLOW | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//renamed.bin; disp=OVERWRITE_IF/OPEN; class=0x01; contexts=DH2Q/MxAc/QFid/RqLs; result=supported=false result=FileNotFoundException | 664–671 (8 SMB2 frames) |
| 20 | `O19_CREATE_HARDLINK` | Создание, проверка и удаление hard link | `OK`, 62.417 ms | CREATE → SET_INFO → CLOSE → CREATE×2 → QUERY_INFO → CREATE → QUERY_INFO → CLOSE×3 → CREATE → CLOSE → CREATE → SET_INFO → CLOSE | 0x00000000 STATUS_SUCCESS; 0x80000005 STATUS_BUFFER_OVERFLOW | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//hardlink.bin, Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//renamed.bin, Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows, Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//copy.bin; disp=OPEN; class=0x01; contexts=MxAc/QFid/RqLs/DH2Q | 675–706 (29 SMB2 frames) |
| 21 | `O20_QUERY_SECURITY` | Чтение security descriptor | `OK`, 78.164 ms | CREATE → QUERY_INFO×2 → CLOSE → CREATE → CLOSE → CREATE → CLOSE → CREATE → QUERY_INFO → CREATE → QUERY_INFO → CLOSE×2 | 0x00000000 STATUS_SUCCESS; 0x80000005 STATUS_BUFFER_OVERFLOW | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//copy.bin, Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//renamed.bin, Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows; disp=OPEN; class=0x01/0x03; contexts=MxAc/QFid/DH2Q/RqLs; result=owner=O:S-1-5-21-1234711004-1596933822-1720474236-1000 access_rules=3 | 710–735 (26 SMB2 frames) |
| 22 | `O21_SET_SECURITY` | Изменение DACL нативной программой icacls | `OK`, 109.2 ms | CREATE → QUERY_INFO → CREATE → QUERY_INFO → CLOSE×2 → CREATE×2 → QUERY_INFO → CLOSE → QUERY_INFO → SET_INFO → CLOSE | 0x00000000 STATUS_SUCCESS | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//renamed.bin, Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows; disp=OPEN; class=0x03; contexts=DH2Q/MxAc/QFid/RqLs; result=icacls_exit=0; processed=1; failed=0 | 739–764 (26 SMB2 frames) |
| 23 | `O22_CHANGE_NOTIFY` | Подписка на изменения, создание файла, получение notify и cancel | `OK`, 640.664 ms | CREATE → QUERY_INFO → CREATE → QUERY_INFO → CREATE → QUERY_INFO → CREATE → CHANGE_NOTIFY → CREATE → WRITE → CHANGE_NOTIFY → QUERY_INFO → CLOSE → CANCEL → CLOSE → CREATE → SET_INFO → CLOSE | 0xc0000034 STATUS_OBJECT_NAME_NOT_FOUND; 0x00000000 STATUS_SUCCESS; 0x00000103 STATUS_PENDING; 0xc0000120 STATUS_CANCELLED | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows, Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//notify.txt, notify.txt; write=6 B; offset=0; disp=OPEN/OVERWRITE_IF; class=0x01; contexts=MxAc/QFid/DH2Q/RqLs; result=change=Created name=notify.txt | 768–802 (30 SMB2 frames) |
| 24 | `O23_DELETE_ON_CLOSE` | Создание файла с delete-on-close | `OK`, 15.613 ms | CREATE → QUERY_INFO → CLOSE → CREATE | 0x00000000 STATUS_SUCCESS; 0xc0000034 STATUS_OBJECT_NAME_NOT_FOUND | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//delete_on_close.tmp; disp=CREATE/OPEN; class=0x01; contexts=DH2Q/MxAc/QFid/RqLs | 806–813 (8 SMB2 frames) |
| 25 | `O24_OPEN_MISSING_EXPECTED` | Открытие отсутствующего файла; ожидаемая ошибка not found | `OK`, 15.66 ms | CREATE | 0xc0000034 STATUS_OBJECT_NAME_NOT_FOUND | disp=OPEN; contexts=DH2Q/MxAc/QFid/RqLs; result=expected=FileNotFoundException | 817–818 (2 SMB2 frames) |
| 26 | `O25_DELETE_NONEMPTY_EXPECTED` | Удаление непустого каталога; ожидаемый отказ | `OK`, 46.882 ms | CREATE×3 → WRITE → QUERY_INFO → CLOSE×2 → CREATE → SET_INFO → CLOSE | 0xc0000034 STATUS_OBJECT_NAME_NOT_FOUND; 0x00000000 STATUS_SUCCESS; 0xc0000101 STATUS_DIRECTORY_NOT_EMPTY | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//nested, Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//nested//child.txt; write=5 B; offset=0; disp=OPEN/CREATE/OVERWRITE_IF; class=0x01; contexts=MxAc/QFid/DH2Q/RqLs; result=expected=IOException | 822–841 (20 SMB2 frames) |
| 27 | `O26_RENAME_DIRECTORY` | Переименование каталога | `OK`, 110.088 ms | CREATE×3 → CLOSE → CREATE → CLOSE → CREATE → CLOSE → SET_INFO → CLOSE | 0x00000000 STATUS_SUCCESS | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//nested, Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows, Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826, Рабочий стол …; disp=OPEN; class=0x01; contexts=DH2Q/MxAc/QFid/RqLs | 845–866 (20 SMB2 frames) |
| 28 | `O27_DELETE_CHILD` | Удаление файла из переименованного каталога | `OK`, 0 ms | CREATE → SET_INFO → CLOSE | 0x00000000 STATUS_SUCCESS | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//nested_renamed//child.txt; disp=OPEN; class=0x01; contexts=DH2Q/MxAc/QFid/RqLs | 870–875 (6 SMB2 frames) |
| 29 | `O28_DELETE_DIRECTORY` | Удаление пустого каталога | `OK`, 15.63 ms | CREATE → CLOSE → CREATE → SET_INFO → CLOSE | 0x00000000 STATUS_SUCCESS | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//nested_renamed; disp=OPEN; class=0x01; contexts=MxAc/QFid/DH2Q/RqLs | 879–888 (10 SMB2 frames) |
| 30 | `O29_DELETE_COPY` | Удаление копии файла | `OK`, 0 ms | CREATE → SET_INFO → CLOSE | 0x00000000 STATUS_SUCCESS | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//copy.bin; disp=OPEN; class=0x01; contexts=DH2Q/MxAc/QFid/RqLs | 897–902 (6 SMB2 frames) |
| 31 | `O30_DELETE_PRIMARY` | Удаление основного файла | `OK`, 0 ms | CREATE → SET_INFO → CLOSE | 0x00000000 STATUS_SUCCESS | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows//renamed.bin; disp=OPEN; class=0x01; contexts=DH2Q/MxAc/QFid/RqLs | 909–914 (6 SMB2 frames) |
| 32 | `O31_DELETE_WORKLOAD_DIRECTORY` | Удаление пустого рабочего каталога | `OK`, 15.508 ms | CREATE → SET_INFO → CLOSE×2 | 0x00000000 STATUS_SUCCESS | name=Рабочий стол//CODEX_SMB_PROTOCOL_TESTS_20260826//workload_windows; disp=OPEN; class=0x01; contexts=DH2Q/MxAc/RqLs | 921–928 (8 SMB2 frames) |
| 33 | `O32_DISCONNECT` | Запрос Windows на отключение от шары | `OK`, 0 ms | — | — | — | — |

## Все статусы ответов

| NTSTATUS | Количество |
|---|---:|
| `0x00000000 STATUS_SUCCESS` | 235 |
| `0xc0000010 STATUS_INVALID_DEVICE_REQUEST` | 11 |
| `0xc0000034 STATUS_OBJECT_NAME_NOT_FOUND` | 11 |
| `0x80000005 STATUS_BUFFER_OVERFLOW` | 8 |
| `0x00000103 STATUS_PENDING` | 5 |
| `0xc000003a STATUS_OBJECT_PATH_NOT_FOUND` | 4 |
| `0x80000006 STATUS_NO_MORE_FILES` | 2 |
| `0xc0000016 STATUS_MORE_PROCESSING_REQUIRED` | 1 |
| `0xc0000033 STATUS_OBJECT_NAME_INVALID` | 1 |
| `0xc0000120 STATUS_CANCELLED` | 1 |
| `0xc0000101 STATUS_DIRECTORY_NOT_EMPTY` | 1 |

## Практические выводы для ZiFi SMB

1. Реализовывать нужно не абстрактную операцию Windows, а всю наблюдаемую цепочку `CREATE/QUERY_INFO/SET_INFO/CLOSE`, включая compound-заголовки, FileId, MessageId, credits и точные NTSTATUS.
2. Успешный `CREATE` Samba сопровождает согласованными leases/oplocks и CREATE contexts; подробные поля каждого ответа находятся в полном decode.
3. `CHANGE_NOTIFY` реально асинхронен: встречаются `STATUS_PENDING`, финальный ответ, `CANCEL` и lease/oplock traffic. Блокировать единственную SMB-задачу ожиданием нельзя.
4. Усечение/расширение, rename, delete-on-close, hardlink и DACL выражаются через разные классы `SET_INFO`; один общий упрощённый ответ не эквивалентен Samba.
5. ADS в текущей эталонной конфигурации без `streams_xattr` не поддержан; это зафиксировано отрицательным сетевым ответом, а не потеряно из таблицы.
6. `WNetCancelConnection2` не обязан немедленно дать `TREE_DISCONNECT/LOGOFF`: Windows сохраняет кэшированный tree/session при открытых namespace handles. Отсутствующие команды отмечены выше как фактический результат.
7. `FileStream.Lock/Unlock` завершился успешно, но отдельный SMB2 `LOCK` в этом одноклиентском workload не появился; Windows обработал его без наблюдаемого wire-LOCK. Для конкурентного lock нужен отдельный двухклиентский сценарий.

## Как проверять отдельную строку

1. Найти ID операции в `udp_markers.csv` и взять START/END frame.
2. Отфильтровать PCAPNG: `frame.number > START && frame.number < END && smb2`.
3. Сверить краткую строку с `smb2_frames.csv`; для всех вложенных структур открыть тот же frame в `smb2_full_decode.txt` или `smb2_packets.json`.
