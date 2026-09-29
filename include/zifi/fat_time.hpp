#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Штампы времени записей FAT и их перевод для сетевых серверов.
//
// Часового пояса в FAT нет: Wild Commander пишет туда показания часов CMOS, а
// Windows — местное время ПК. Протоколам же нужен UTC: FTP MDTM/MFMT/MLSD и
// SMB FILETIME. Поэтому штамп переводится с поясом из zifi.ini (time:, целые
// часы) — тем же, с которым ESP сверяет часы по NTP.
//
// Дата 0 означает «неизвестно»: так её отдают старый плагин и старое ядро WC.
struct FatStamp {
  uint16_t date = 0;
  uint16_t time = 0;

  bool known() const { return date != 0; }
};

// Штампы одной записи каталога. Изменение известно, когда его отдал плагин
// (листинг или STAT); создание и доступ — только из STAT нового плагина
// (FILEX GET_METADATA), поэтому отмечены отдельным признаком.
struct FatTimes {
  FatStamp write;
  bool full = false;
  FatStamp create;
  uint8_t createTenth = 0;  // сотые доли секунды создания, 0..199
  uint16_t accessDate = 0;
};

// Гражданское время <-> секунды от 1970-01-01. Пояс здесь не учитывается:
// поля трактуются как UTC. Годы 1..9999, високосные по григорианскому правилу.
int64_t civilToUnix(int year, int month, int day, int hour, int minute,
                    int second);
void unixToCivil(int64_t unixSeconds, int& year, int& month, int& day,
                 int& hour, int& minute, int& second);

// Разложить штамп на поля. false — дата неизвестна или поля вне календаря
// (месяц 13, 30 февраля, 25 часов): такой штамп не показываем как дату.
bool fatStampToCivil(FatStamp stamp, int& year, int& month, int& day,
                     int& hour, int& minute, int& second);

// Местный штамп FAT -> секунды UTC. false — как у fatStampToCivil.
bool fatStampToUnix(FatStamp stamp, int32_t timezoneSeconds,
                    int64_t& unixSeconds);

// Секунды UTC -> местный штамп FAT. Секунды FAT хранит с шагом 2, нечётная
// отбрасывается. false — вне диапазона FAT: 1980-01-01..2107-12-31 местного
// времени. Такую дату записывать нельзя, её ставит сам WC (текущая).
bool unixToFatStamp(int64_t unixSeconds, int32_t timezoneSeconds,
                    FatStamp& stamp);

// «YYYYMMDDhhmmss» (UTC) — значение времени FTP (RFC 3659: MDTM, MFMT, MLSD).
// Разбор допускает дробную часть «.sss» и отбрасывает её; end указывает сразу
// за разобранным. false — не тот формат или невозможная дата.
bool parseFtpTimeVal(const char* text, int64_t& unixSeconds,
                     const char** end = nullptr);
// output — 15 байт: 14 цифр и ноль.
void formatFtpTimeVal(int64_t unixSeconds, char* output);

// Windows FILETIME: сотни наносекунд от 1601-01-01 UTC.
uint64_t unixToFileTime(int64_t unixSeconds);
// false — ноль, «не менять» (все единицы) или время раньше 1970.
bool fileTimeToUnix(uint64_t fileTime, int64_t& unixSeconds);

}  // namespace zifi
