# SD→SD: CREATE каталога во время активного FINDNEXT

## Наблюдение на ZX Evolution

В Проводнике Windows копирование каталога внутри `\\ZX-Evo\SD` завершилось
сообщением «вам необходимо разрешение на выполнение этой операции».
Состояние до отмены операции сохранено в захватах:

- `captures/sd_to_sd_active_hang_0677_20260827-230004.etl`;
- `captures/sd_to_sd_active_hang_0677_20260827-230004.pcapng`.

В захвате запрос `CREATE 111\wc`, MessageId 1012, содержит:

- DesiredAccess `0x00100081`;
- FileAttributes `0x00000080`;
- ShareAccess `0x00000003`;
- CreateDisposition `0x00000002` (`FILE_CREATE`);
- CreateOptions `0x00200021` (`DIRECTORY_FILE | SYNCHRONOUS_IO_NONALERT |
  OPEN_REPARSE_POINT`).

Сервер ответил `STATUS_ACCESS_DENIED` (`0xC0000022`) через 9,764 мс. Следующий
`OPEN` того же пути получил `STATUS_OBJECT_NAME_NOT_FOUND` (`0xC0000034`), то
есть каталог физически не создавался. В это время более ранний
`QUERY_DIRECTORY`, MessageId 1003, уже выполнял `FINDNEXT` и завершился примерно
через 5,5 с после отказа `CREATE`.

## Причина и исправление

До `s3-native-0.6.78` новый каталог выполнял синхронные `STAT`, `closeActive`
и `MKDIR` прямо из обработчика `CREATE`. Единственный FILEX-мост уже принадлежал
`QUERY_DIRECTORY`, поэтому внутренний `bridge-busy` превращался в
`STATUS_IO_TIMEOUT` или `STATUS_ACCESS_DENIED`.

В `s3-native-0.6.78` весь `FILE_CREATE` каталога, включая проверочный `STAT` и
`MKDIR`, удерживается как внутренне-асинхронный запрос SMB и выполняется по общей
FIFO вместе с READ, WRITE и холодным QUERY_DIRECTORY. MessageId остаётся в
`waitqueue` до финального синхронного ответа на проводе.

## Регрессия и границы проверки

`tools/host_smb/smb_reproduce_test.c`, TEST 32, повторяет точные поля `CREATE`,
оставляет `FINDNEXT` активным при искусственной задержке 250 мс и требует
успешного ответа только после завершения старшей операции. Результат после
исправления: `STATUS_SUCCESS`, 547 мс; параллельный `QUERY_DIRECTORY` также
завершён успешно. Полный host-набор TEST 1–31 и отдельный PDU-timeout тест тоже
прошли.

Чистая локальная сборка `s3-native-0.6.78` успешна. Это ещё не доказательство
работы SD→SD на реальном ZX Evolution: аппаратная проверка начинается только
после отдельной установки новой прошивки.
