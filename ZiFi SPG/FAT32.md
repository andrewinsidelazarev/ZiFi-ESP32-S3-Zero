# Самостоятельный FAT32

`zifi.spg` использует драйвер из отдельного репозитория
[FAT32-Driver-ZX-Evolution](https://github.com/andrewinsidelazarev/FAT32-Driver-ZX-Evolution)
(лицензия MIT). Его клонируют рядом с этим репозиторием, в папку `FAT32 Driver`:

```
git clone https://github.com/andrewinsidelazarev/FAT32-Driver-ZX-Evolution.git "FAT32 Driver"
```

От `ZiFi SPG` это путь `../../FAT32 Driver`.
Сборка `build.bat` сначала собирает драйвер, затем включает его код на
страницу `#0E`, рабочую RAM и SD-ZC — на `#0F`. Другой путь к драйверу
задаётся переменной `FAT32_DRIVER_DIR`.

Чтение `zifi.ini`, каталоги загрузок и сохранение файлов проходят через
`fat32_adapter.asm`. Для записи используется APPEND (API76) с точным
размером, проверкой границ 640-КиБ буфера и восстановлением банков памяти.
Адрес источника задаётся в окне `#4000`; отдельный музыкальный буфер занимает
страницы `#1D..#1E` и ограничен 32 КиБ.
Существующий файл сохраняется; при ошибке запись может остаться частичной,
о чём ZiFi сообщает в журнале.

API и сборка описаны в [README драйвера](https://github.com/andrewinsidelazarev/FAT32-Driver-ZX-Evolution#readme).
Результаты тестов — в [отчёте проверки](https://github.com/andrewinsidelazarev/FAT32-Driver-ZX-Evolution/blob/main/VALIDATION.md).
Проверены выполнение Z80, автономный драйвер и сквозная загрузка сетевого
клиента в отдельной исправленной сборке Unreal: HTTP → UART → Z80 → FAT32 →
образ SD на файлах до 640 КиБ с побайтной сверкой. Проверка на физическом
ZX-Evolution остаётся следующим этапом.
