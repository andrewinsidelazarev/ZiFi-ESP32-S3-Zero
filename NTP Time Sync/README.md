# ZiFi ESP32-S3 Zero NTP Time Sync

Плагин Wild Commander Improved типа `#06` (исполняемый при запуске WC) : после инициализации WC загружает
`/zifi/zifi.ini`, передаёт его ESP32-S3-Zero командой `WIFI_INI`, запрашивает
`NET_NTP` и при расхождении не менее минуты обновляет RTC Mr.Gluk.

Формат ответа прошивки: ровно 14 ASCII-цифр `YYYYMMDDhhmmss`, уже с
часовым поясом `time:` из `zifi.ini` (смещение +- относительно UTC (GMT)). При любой ошибке плагин молча возвращает
управление Wild Commander и не меняет часы.

## Сборка

```powershell
& ".\NTP Time Sync\build.bat"
```

Результат: `build/NTPTIME.WMF`. Сборка использует только файлы текущего проекта:
`src` и `../shared/z80`.

## Установка

Скопируйте `NTPTIME.WMF` в каталог плагинов Wild Commander Improved. Плагин
типа `#06` выполняется один раз после полной инициализации WC.

