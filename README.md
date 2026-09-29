# ZiFi ESP32-S3 Zero

<p align="center">
  <img src="Zifi%20ESP32%20Zero%20Adapter/Documentation/esp32-s3-zero-to-esp-01-adapter.webp" alt="ZiFi ESP32-S3 Zero to ESP-01S Adapter" width="700">
</p>

Нативная прошивка для стандартного Waveshare ESP32-S3-Zero на
`ESP32-S3FH4R2` (4 МБ Flash, 2 МБ QSPI PSRAM). Модуль устанавливается вместо
ESP-01S через переходник и сохраняет двоичный UART-протокол ZiFi.

Текущая версия реализует двоичный протокол ZiFi, NTP, HTTP-транспорт
`zifi.spg`, FTP и SMB2/SMB3 поверх двухъядерного VFS-моста:

- UART0: `RX=GPIO44`, `TX=GPIO43`, 115200 8N1;
- команды `ECHO`, `WIFI_CONNECT`, `WIFI_INI`, `PING`, `SYS_INFO`, `GET_STEP`,
  `FTP_START/STOP`, `FTP_RAM_STATS`, `SMB_START/STOP`, `NET_OPEN/SEND/RECV/CLOSE`,
  `NET_HTTP_GET`, `NET_PING`, `NET_IP_CONFIG`, `NET_NTP`, `NET_PROXY_STATUS`,
  `WEATHER_GET`, `UPDATE_START/STOP` и `SYS_RESET`;
- прозрачный HTTP-прокси для обхода блокировок ретро-архивов (`vtrd.in`, `zxart.ee` и др.) с проверкой доступности и фиксированной авторизацией;
- прямой HTTPS на ESP32 с проверкой сервера по встроенному Mozilla CA-bundle;
  тело ответа передаётся Z80 без перекодирования и распаковки;
- NTP-плагин Wild Commander с результатом `YYYYMMDDhhmmss`;
- погода для заставок Wild Commander (`WEATHER_GET`): город (или почтовый
  индекс) из `zifi.ini` → координаты (геокодер Open-Meteo, индекс — zippopotam.us)
  → прогноз Open-Meteo, Z80 получает готовую запись на 90 байт;
- совместимость с Native-версией `zifi.spg`: ESP сама выполняет DNS,
  HTTP-запрос и разбор заголовка, Z80 забирает тело командами `NET_RECV`;
- FTP с тремя управляющими сессиями на задаваемом порту (обычно `21`),
  active `PORT/EPRT` и отдельными passive `PASV/EPSV`-портами `2122–2124`;
- односессионный SMB2/SMB3 на TCP/445 с NTLMSSP; signing объявлена доступной,
  но сервер её не требует (`SecurityMode=1`), ресурсом по номеру устройства WC,
  ответами NBNS по UDP/137 и WS-Discovery по UDP/3702 для сетевого окружения Windows 10/11;
- мгновенный отклик при открытии шары без блокирующего 60-секундного сканирования FAT;
- надёжное сохранение и перезапись файлов (`FILE_OVERWRITE_IF` / `FILE_SUPERSEDE`):
  прямой последовательный тракт (Mode 1 / `APPEND`) с корректным выделением кластеров в FAT32
  Wild Commander и неблокирующим обновлением метаданных времени;
- VFS-клиент Wild Commander (`STAT`, каталоги, чтение, блочная запись,
  `CLOSE`, удаление, создание каталогов, append, увеличение EOF нулями и
  переименование);
- два межъядерных кольца по 64 КиБ в PSRAM для `Network -> VFS` и
  `VFS -> Network`;
- общий пул из восьми 64-КиБ рабочих PSRAM-слотов для SMB READ/WRITE и
  скользящее READ-окно: один физически обслуживаемый async-запрос, а следующие
  маленькие дескрипторы удерживают credits клиента до своего продвижения;
- сохранение полного `zifi.ini` в LittleFS только при изменении CRC;
- A/B-обновление по Wi-Fi с проверкой application-заголовка ESP32-S3,
  SHA-256 и автоматическим rollback;
- полный factory-образ для первой записи через USB.

WebDAV ещё не включён. Сетевой файловый listener запускается только явной
командой плагина: `FTP_START` для `ZIFIFTP.WMF` либо `SMB_START` для
`ZIFISMB.WMF`. Одно наличие Wi-Fi не открывает порты `21` и `445`.

## Готовые файлы для скачивания

| Категория | Файл для скачивания | Назначение |
|---|---|---|
| **Первая прошивка (USB)** | [**`firmware.factory.bin`**](firmware/firmware.factory.bin) | Полный 4 МБ образ для первой прошивки ESP32-S3 через USB Type-C (смещение `0x0`) |
| **Проверка без Wi-Fi** | [**`esp_info.sna`**](tools/esp_info.sna) | Программа для ZX Evolution: проверяет запуск прошивки и UART-связь с ESP до настройки `zifi.ini` и Wi-Fi |
| **Обновление по Wi-Fi (OTA)** | [**`firmware.bin`**](firmware/firmware.bin) | Образ приложения для сетевого обновления ([SHA-256](firmware/firmware.sha256)) |
| **Автономный апдейтер** | [**`ZIFIUPD.WMF`**](Online%20Update/build/ZIFIUPD.WMF) | Пользовательский плагин Wild Commander: читает `zifi.ini`, скачивает и устанавливает прошивку с GitHub |
| **Режим обновления с PC** | [**`Update Mode`**](Update%20Mode/src/update.asm) | Исходник локально собираемого `update.sna`, запускающего приём OTA на порту 8267 |
| **Скрипт-прошивальщик (OTA)** | [**`esp_tool.py`**](tools/esp_tool.py) | Python-утилита для прошивки ESP32-S3 по Wi-Fi и выгрузки логов |
| **SMB-сервер (Windows)** | [**`ZIFISMB.WMF`**](SMB%20Server/build/ZIFISMB.WMF) | Плагин Wild Commander v0.5.10: доступ к текущему тому SD/IDE, например `\\ZX-Evo\0` |
| **FTP-сервер** | [**`ZIFIFTP.WMF`**](FTP%20Server/build/ZIFIFTP.WMF) | Плагин Wild Commander: полнофункциональный FTP-сервер |
| **Синхронизация времени** | [**`NTPTIME.WMF`**](NTP%20Time%20Sync/build/NTPTIME.WMF) | Плагин Wild Commander: синхронизация часов RTC через интернет |
| **Проверка и обновление WC** | [**`WCUPDATE.WMF`**](WC%20Update/build/WCUPDATE.WMF) | Плагин Wild Commander: сверяет файлы WC на SD с GitHub по SHA и обновляет отличающиеся (прошивка `s3-native-0.6.94` или новее) |
| **Заставка с погодой** | [**`WEATHER.WMF`**](Weather%20screensaver/build/WEATHER.WMF) | Заставка Wild Commander: часы, календарь, погода и прогноз на 5 дней (прошивка `s3-native-0.6.93` или новее) |
| **Заставка с погодой для VDAC2** | [**`WEATHER2.WMF`**](Weather%20screensaver%20VDAC2/build/WEATHER2.WMF) | Та же заставка в 1024×768 на видеочипе FT812 платы VDAC2 (прошивка `s3-native-0.6.93` или новее) |
| **Браузер / Загрузчик** | [**`zifi.spg`**](ZiFi%20SPG/build/zifi.spg) ([пример `zifi.ini`](ZiFi%20SPG/build/zifi.ini)) | Программа ZiFi для ZX-Evolution (каталог сайтов, скачивание) |
| **Печатная плата переходника** | [**`Manufacturing.zip`**](Zifi%20ESP32%20Zero%20Adapter/Zifi_ESP32_Zero_Adapter-Manufacturing.zip) | Готовый архив герберов для заказа платы переходника в производство |

## Архитектура двух ядер

```text
core 1: UART0 + frame parser + VFS client Wild Commander
       ▲                         ▲                │
       │ control queues          │ 64 KiB PSRAM   │ 64 KiB PSRAM
       ▼                         │ Network -> VFS │ VFS -> Network
core 0: Wi-Fi events + DNS + TCP/HTTP + FTP/SMB/OTA + UDP/NTP/NBNS
```

- UART принадлежит только core 1: сетевой worker в него не пишет, поэтому
  ответы разных задач не перемешивают кадры. События Wi-Fi Arduino закреплены
  за core 0.
- Управляющие очереди передают однобайтовые сигналы, данные — два SPSC-кольца
  в PSRAM. Если PSRAM не запустилась, есть диагностический запасной вариант по
  4 КиБ во внутренней RAM.
- FTP и SMB передают файлы между кольцом и Z80 окнами до 16 КиБ: UART-кадры до
  1024 байт идут без промежуточных ACK, а окно подтверждается после CRC и
  записи `APPEND`. Старые команды по 512 байт оставлены для совместимости.
- Пока core 1 ждёт медленную запись SD, сеть продолжает принимать данные в
  PSRAM; обратное направление идёт через отдельное кольцо. `SYS_INFO`
  показывает ядра, тип памяти и размер обоих колец.

SMB-запросы Windows (READ до 64 КиБ, WRITE до 512 КиБ) core 1 проводит через
физические окна Wild Commander по 16 КиБ; каталожный кэш занимает отдельную
область PSRAM. Как сервер подтверждает запись и держит тайм-ауты Windows —
в разделе [«Сборка SMB-плагина»](#сборка-smb-плагина).

## Программы и плагины Z80

* **`ZiFi SPG` (`ZiFi SPG/build/zifi.spg`):**
  Полноценный браузер и загрузчик для ZX-Evolution (исходники на Z80 и сборка SPG в `ZiFi SPG/`).
  Работает через двоичные команды `NET_HTTP_GET`, `NET_RECV`, `NET_CLOSE`, `WIFI_INI` и `NET_NTP`.
  ZIP-ссылки сначала проходят через серверный `unzipremote.php`: корректный
  распакованный файл сохраняется сразу, а ложный `.zip` без сигнатуры `PK`
  автоматически повторно загружается напрямую как исходный ZIP-архив.
* **`SMB Server` (`SMB Server/build/ZIFISMB.WMF`):**
  Плагин для Wild Commander, открывающий текущий том SD/IDE по SMB2/SMB3. Имя ресурса совпадает с номером устройства в панели WC: например, `0:\` доступен как `\\ZX-Evo\0`.
* **`FTP Server` (`FTP Server/build/ZIFIFTP.WMF`):**
  Плагин для Wild Commander v0.15 (команда `FTP_START`), запускающий FTP-сервер с поддержкой активного и пассивного режимов и отдельной цветной шкалой Wi-Fi; даты файлов — как в панели WC; после записи по FTP панели WC перечитываются при выходе.
* **`NTP Time Sync` (`NTP Time Sync/build/NTPTIME.WMF`):**
  Плагин для Wild Commander для сетевой синхронизации часов реального времени RTC.
* **`WC Update` (`WC Update/build/WCUPDATE.WMF`):**
  Аналог `sfc /scannow` для Wild Commander Improved: ESP сверяет файлы WC на
  SD с каталогом `exe` на GitHub по git-SHA и обновляет отмеченные с проверкой
  SHA после скачивания, записи во временный файл и замены; `wc.ini` не
  перезаписывается. Подробности — [`WC Update/README.md`](WC%20Update/README.md).
* **`Weather screensaver` (`Weather screensaver/build/WEATHER.WMF`):**
  Заставка Wild Commander (тип `#02`) на экране 360×288: место, крупные часы с
  мигающим двоеточием, дата, текущая погода, прогноз на 5 дней и лента недели.
  Место — город по-английски в `/zifi/zifi.ini` (`city: Kyiv`, можно с
  `country:`) или, как раньше, `country:` и `zip:`; погоду раз в час
  запрашивает командой `WEATHER_GET`. Подробности — в
  [описании заставки](Weather%20screensaver/README.md).
* **`Weather screensaver VDAC2` (`Weather screensaver VDAC2/build/WEATHER2.WMF`):**
  Та же заставка для платы VDAC2: экран 1024×768 @ 59 Гц рисует видеочип
  FT812, текст сглажен, панели полупрозрачные. Главный цикл и обмен с ESP —
  общие с `WEATHER.WMF` ([`shared/weather`](shared/weather)). Подробности — в
  [описании](Weather%20screensaver%20VDAC2/README.md).
* **`Online Update` (`Online Update/build/ZIFIUPD.WMF`):**
  Ручной пользовательский обновлятор: читает `/zifi/zifi.ini`, подключает ESP к
  Wi-Fi, показывает установленную и опубликованную версии, затем по подтверждению
  запускает прямую HTTPS-загрузку `firmware.sha256` и `firmware.bin` из GitHub.
  PC и Python не нужны; повторная установка той же версии разрешена.
* **`Update Mode` (`Update Mode/src/update.asm`):**
  Исходник совместимого обновления с PC через `tools/esp_tool.py`. Snapshot
  `firmware/update.sna` создаётся локально сборкой и не публикуется в Git.


## Сборка прошивки

```powershell
# Выполняйте из корня репозитория.
python -m pip install -r requirements-build.txt
.\build.bat
```

`build.bat` вычисляет короткий ASCII-путь к кэшу PlatformIO. Это обход
ограничения Xtensa GCC 8.4, который не открывает Arduino framework из
Unicode-пути профиля Windows; сам проект и пакеты при этом никуда не копируются.

PlatformIO автоматически создаёт:

- `firmware/firmware.bin` — приложение по адресу `0x10000`;
- `firmware/firmware.factory.bin` — объединённый образ для записи с `0x0`;
- `firmware/firmware.sha256` — опубликованная версия и SHA-256 обоих образов;
- `firmware/update.sna` — локальный игнорируемый результат сборки: экран
  включения сетевого updater на порту `8267`.

Параметры платы закреплены в `platformio.ini`: официальный совместимый профиль
`esp32-s3-devkitm-1` переопределён под 4 МБ QIO Flash и 2 МБ QSPI PSRAM.

## Сборка NTP-плагина

```powershell
& ".\NTP Time Sync\build.bat"
```

Результат: `NTP Time Sync/build/NTPTIME.WMF`. Общие ASM-файлы находятся в
`shared/z80`; проект не зависит от соседних каталогов ZiFi.

## Сборка плагина WC Update

```powershell
& ".\WC Update\build.bat"
```

Результат: `WC Update/build/WCUPDATE.WMF`. Файловые запросы ESP плагин
обслуживает модулями FTP-плагина (`FTP Server/src/vfs.asm`, `fs.asm`), общие
файлы — из `shared/z80`.

## Сборка FTP-плагина

```powershell
& ".\FTP Server\build.bat"
```

Результат: `FTP Server/build/ZIFIFTP.WMF` v0.15.

- Даты файлов — как в панели WC (`LIST`, `MLSD`, `MDTM`, запись `MFMT`). Нужны
  прошивка `s3-native-0.6.94` и
  [Wild Commander Improved v1.11i от 28 сентября 2026 года](https://github.com/andrewinsidelazarev/Wild-Commander-Improved/releases/tag/v1.11i-2026-09-28)
  или новее; с более старыми даты просто не показываются.
- Каталог листается пачками по 16 записей; окно стоит по центру экрана в любом
  текстовом режиме WC.
- Поле `Status` показывает 16-сегментную шкалу Wi-Fi и точный процент: клетки и
  процент зелёные от 60 %, жёлтые от 30 %, ниже — красные.
- После записи по FTP Wild Commander перечитывает панели.
- Файлы идут окнами до 16 КиБ, CRC-16 окна считается по таблице.

Общие ASM-файлы берутся из `shared/z80`. Подробности — в
[описании плагина](FTP%20Server/README.md).

## Сборка SMB-плагина

```powershell
& ".\SMB Server\build.bat"
```

Результат: `SMB Server/build/ZIFISMB.WMF` v0.5.10. Нужны прошивка
`s3-native-0.6.94` и [Wild Commander Improved v1.11i от 28 сентября 2026 года](https://github.com/andrewinsidelazarev/Wild-Commander-Improved/releases/tag/v1.11i-2026-09-28) или новее.

Что умеет плагин:

- открывает корень тома активной панели WC как сетевой ресурс с номером
  устройства, например `\\ZX-Evo\0`; номер виден в полях `Share` и `UNC`, а
  выбранный раздел сохраняется, даже если `/zifi/zifi.ini` найден на другой
  SD-карте;
- даты файлов — как в панели WC;
- быстро открывает папки: записи каталога идут по UART пачками по 16;
- показывает цветную шкалу Wi-Fi в поле `Status`;
- при выходе WC перечитывает обе панели.

Настройки по умолчанию: порт `445`, имя `ZX-Evo`, группа `WORKGROUP`, логин и
пароль `zx` / `zx`. Для записи нужен `WC/FILEX.WMF` первой активной строкой
`[PLUGINS]` в `wc.ini`.

Как сервер работает с Windows — коротко:

- **Надёжность.** READ и WRITE подтверждаются только после записи на SD,
  окнами FILEX по 16 КиБ. Долгий запрос получает промежуточный
  `STATUS_PENDING`: Windows не обрывает его через 60 секунд, а «Отмена»
  срабатывает.
- **Ход копирования.** Поле `Copying` считает уникальные переданные куски.
  Windows пишет их не по порядку, но шкала не откатывается.
- **Подпись.** Доступна, но не обязательна (`SecurityMode=1`): Windows сама
  выбирает неподписанный режим.
- **Обрыв связи.** Потерянные пакеты повторяет TCP, Wi-Fi переподключается
  сам. Прерванный SMB-сеанс не возобновляется — копирование нужно начать
  заново.
- **Папки.** Каталог читается с SD один раз в снимок в PSRAM (до 512 КиБ), и
  Проводник листает уже его. Изменение с Evo сбрасывает снимок только своей
  папки.

Подробная инструкция, устройство и ограничения — в
[описании плагина](SMB%20Server/README.md).

## Сборка заставки погоды

```powershell
& ".\Weather screensaver\build.bat"
```

Результат: `Weather screensaver/build/WEATHER.WMF`. Шрифты, иконки и палитру
генерирует `tools/gen_assets.py` из системных шрифтов Windows (Segoe UI,
Tahoma, Segoe UI Emoji), поэтому нужны Python 3 с Pillow. Машинные тесты
исполняют плагин в эмуляторе Z80 и сравнивают кадр с эталоном попиксельно —
см. [описание заставки](Weather%20screensaver/README.md).

Заставка для VDAC2 собирается так же:

```powershell
& ".\Weather screensaver VDAC2\build.bat"
```

Результат: `Weather screensaver VDAC2/build/WEATHER2.WMF`. Её тесты рисуют кадр
эмулятором FT812 `bt8xxemu.dll` (каталог задаёт `BT8XXEMU_DIR`) — см.
[описание](Weather%20screensaver%20VDAC2/README.md).

## Первая прошивка через USB

Для первоначальной прошивки нового модуля Waveshare ESP32-S3-Zero используется кабель USB Type-C и полный заводской образ [**`firmware.factory.bin`**](firmware/firmware.factory.bin) (записывается со смещения `0x0`).

> [!WARNING]
> **Внимание!** Перед подключением кабеля USB-C обязательно извлеките плату переходника из слота ZX-Evolution (либо отключите питание Спектрума). Источники питания не развязаны.

Подробная пошаговая инструкция со ссылкой на официальный `esptool`, полным
стиранием Flash и обязательным режимом записи `-fm dio` находится в
[`firmware/README_RU.md`](firmware/README_RU.md#первая-установка-через-usb-c).

### Альтернативный способ через PlatformIO

```powershell
pio run -t upload
```

После первой успешной прошивки модуль готов к установке в слот Спектрума, а все последующие обновления выполняются по Wi-Fi.

## Обновление без USB

USB-C нужен только для первой установки `firmware.factory.bin`. После того как
прошивка хотя бы один раз получила `WIFI_INI` от совместимой Native-программы,
полный `zifi.ini` сохранён в LittleFS и сеть восстанавливается после reset.

Для обычного пользователя предназначен `Online Update/build/ZIFIUPD.WMF`:

1. Один раз установите прошивку `s3-native-0.6.69` или новее прежним способом,
   поскольку более старые версии ещё не поддерживают раздельные команды проверки
   и установки.
2. Скопируйте `ZIFIUPD.WMF` в `/WC` и запускайте его из меню плагинов Wild
   Commander. Файл `/zifi/zifi.ini` должен содержать действующие параметры Wi-Fi.
3. Плагин без записи во Flash покажет `Current`, `Available` и результат
   сравнения. Нажмите `Enter` для установки. При `Same version` Enter выполняет
   восстановительную переустановку того же опубликованного образа.
4. ESP сама загрузит по проверяемому HTTPS сначала `firmware.sha256`, затем
   `firmware.bin` из GitHub `main`, сверит полный SHA-256, переключит неактивный
   OTA-слот и перезапустится. PC, IP-адрес и Python пользователю не требуются.

До совпадения SHA-256 текущий загрузочный слот не меняется. При обрыве,
неверном образе или ошибке записи вызывается `Update.abort()`. После успешного
перезапуска плагин повторно читает `SYS_INFO` и показывает запущенную версию.

Актуальный пользовательский плагин можно
[скачать как `ZIFIUPD.WMF` напрямую с GitHub](https://github.com/andrewinsidelazarev/ZiFi-ESP32-S3-Zero/raw/refs/heads/main/Online%20Update/build/ZIFIUPD.WMF).
Подробный порядок первой USB-прошивки и последующих обновлений через GitHub
описан в [`firmware/README_RU.md`](firmware/README_RU.md).

## Аппаратное предупреждение

Переходник подаёт 3,3 В основной платы непосредственно на контакт `3V3`
ESP32-S3-Zero. Нельзя подключать USB-C, пока переходник установлен в запитанную
основную плату: источники питания не развязаны.

Параметры модуля и рекомендация PlatformIO:
[Waveshare ESP32-S3-Zero](https://www.waveshare.com/wiki/ESP32-S3-Zero).

## Благодарности и сторонние компоненты

* **[libsmb2](https://github.com/sahlberg/libsmb2)** — автор Ronnie Sahlberg (LGPL-2.1). Открытая клиентская и серверная реализация протоколов SMB2/SMB3 и RPC/srvsvc.

