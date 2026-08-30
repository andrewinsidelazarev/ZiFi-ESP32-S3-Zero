#include "zifi/smb_server.hpp"
#include "zifi/directory_cache.hpp"
#include "zifi/ws_discovery.hpp"
#include "zifi/diagnostic_log.hpp"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include <ctype.h>
#include <errno.h>
#include <new>
#include <sys/poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

// Важно соблюдать этот порядок: libsmb2.h использует типы, объявленные в
// smb2.h. Заголовки написаны на C, поэтому окружаем их C-связыванием.
extern "C" {
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-dcerpc-server.h>
#include <smb2/smb2-errors.h>
#include <smb2/smb2-ioctl.h>
// Внутренний заголовок объявляет функцию с enum dcerpc_encoding в результате.
// C++ требует увидеть определение enum раньше этого объявления.
#include <dcerpc/dcerpc.h>
#include "libsmb2-private.h"
}

// Arduino-ядро ESP32 не предоставляет реализацию gethostname(), хотя
// серверная часть libsmb2 ссылается на неё как на запасной вариант.
// В нативной сборке симулятора функция уже есть в системных библиотеках.
#ifndef ZIFI_HOST_BUILD
extern "C" int gethostname(char* name, size_t length) {
  if (name == nullptr || length == 0) {
    errno = EINVAL;
    return -1;
  }
  snprintf(name, length, "ZiFi");
  return 0;
}
#endif  // ZIFI_HOST_BUILD

namespace zifi {
namespace {

constexpr uint16_t kDefaultPort = 445;
constexpr size_t kMaxPath = 255;
constexpr size_t kHandleCount = 8;
// FAT-метаданные меняются редко. Шестнадцати записей хватает для всех
// одновременно открытых объектов и последних файлов Проводника, занимая
// около четырёх килобайт вместо ещё одного большого кэша каталогов.
constexpr size_t kMetadataCacheCount = 16;
// Byte-range locks принадлежат SMB Open, а не физическому FILEX-дескриптору.
// Тридцать две записи занимают около одного килобайта внутренней памяти и
// ограничивают расход памяти независимо от поведения клиентов.
constexpr size_t kByteRangeLockCount = 32;
constexpr size_t kTreeCount = 8;
constexpr size_t kSrvsvcResponseCapacity = 1024;
constexpr uint32_t kFirstTreeId = 0x5A580001UL;
constexpr uint32_t kNormalVfsTimeoutMs = 10000;
constexpr uint32_t kMutateVfsTimeoutMs = 190000;
// Как часто обновляется строка хода передачи. Четыре обновления в секунду
// человек читает свободно, а UART при этом остаётся занят данными файла.
constexpr uint32_t kProgressIntervalMs = 250;
// Сколько ждать освобождения единственного канала к SD, прежде чем признать
// его занятым. Окно 16 КиБ уходит за единицы миллисекунд, а Windows штатно
// терпит задержку ответа на CREATE секундами — так что запас здесь дешёвый.
constexpr uint32_t kBridgeWaitMs = 2000;
constexpr uint32_t kSmbTaskStackBytes = 24 * 1024;
constexpr UBaseType_t kSmbTaskPriority = 2;
constexpr BaseType_t kSmbTaskCore = 0;
// TCP-очередь может содержать несколько коротких подключений Windows, хотя
// файловый VFS ниже по-прежнему обслуживает только один активный SMB-сеанс.
constexpr int kSmbListenBacklog = 4;
constexpr uint32_t kSmbListenerRetryMs = 250;
// SMB-клиенты ОС ожидают, что сервер объявит не менее 64 КиБ. Это сетевой
// размер запроса; физический тракт Wild Commander по-прежнему работает окнами
// до 16 КиБ. READ и WRITE обязаны подтвердить полный сетевой запрос: реальный
// Windows CopyFile не повторяет остаток короткого успешного ответа. Физические
// окна остаются только внутренними этапами одного асинхронного SMB-запроса.
constexpr uint32_t kSmbAdvertisedTransactSize = 64 * 1024;
constexpr uint32_t kSmbAdvertisedReadSize = 64 * 1024;
// CopyFileEx / IFileOperation передают запись Windows порциями по 512 КиБ.
// Если MaxWrite меньше, MS-SMB2 разрешает redirector дробить одну такую порцию
// и отправлять части в любом порядке. При старом MaxWrite=64 КиБ это реально
// давало порядок 0, 512K..4M, 64K..512K: клиентский callback стоял на 0%, а
// затем прыгал сразу на десятки процентов. Принимаем порцию целиком, а внутри
// всё равно пишем её через короткие окна FILEX.
constexpr uint32_t kSmbAdvertisedWriteSize = 64 * 1024;
// ЗАПРЕЩЕНО уменьшать: по MS-SMB2 3.2.5.2 клиент обязан разорвать соединение,
// если для диалекта 0x0210 и выше сервер объявил MaxRead/MaxWrite/MaxTransact
// меньше 65536. Windows делает это буквально — Проводник показывает «Параметр
// задан неверно» и ресурс не открывается вовсе. Соблазн объявить окно поменьше
// ради отзывчивости возникает регулярно; цена ему — неработающий ресурс.
// Файл, прочитанный целиком с начала, оседает в PSRAM: Проводник при
// копировании читает исходный файл дважды подряд, и второй проход обязан
// обойтись без единого байта по UART. Кэшируется только то, после чего в
// PSRAM остаётся запас на буферы сети и каталогов.
// 640 КиБ — это ровно образ TRD, самое частое, что копируют с карты ZX.
constexpr uint32_t kFileCacheLimit = 640 * 1024;
constexpr size_t kFileCachePsramFloor = 320 * 1024;
// Медленный READ остаётся внутренне асинхронным и больше не подчиняется
// обычному PDU timeout; watchdog контролирует отсутствие физического прогресса
// самостоятельно.
// WRITE_THROUGH удерживает свой SMB-кредит до физического завершения.
constexpr uint32_t kAsyncIoProgressTimeoutMs = 90 * 1000;
// До этой границы Windows успевает получить штатный промежуточный ответ раньше
// своего обычного 60-секундного тайм-аута файлового запроса. STATUS_PENDING не
// подтверждает данные: окончательный SUCCESS по-прежнему приходит только после
// физического FILEX I/O.
constexpr uint32_t kLongIoInterimPendingMs = 30 * 1000;
// Восемь слотов соответствуют восьми SMB credits. Слоты стартуют по 64 КиБ и
// растут до фактической длины WRITE только по мере необходимости: база занимает
// 512 КиБ PSRAM, а не прежний 1 МиБ, оставляя место входному 512-КиБ PDU.
constexpr size_t kAsyncIoQueueDepth = SMB2_SERVER_CREDIT_TARGET;
// Физически обслуживаем только один READ. Быстрые запросы удерживают credit до
// финала; запрос старше 30 секунд получает STATUS_PENDING, но сервер перед этим
// снижает credit target соединения до одного. Поэтому Windows продлевает
// тайм-аут, а прежняя бесконечно самопополняющаяся очередь не возвращается.
// Это flow control, а не предел размера файла; backend FILEX сериализован.
constexpr size_t kAsyncReadServiceDepth = 1;
// Размер реестра соединений. Это НЕ предел: соединение сверх реестра всё
// равно обслуживается, просто не попадает в учёт. Один Dolphin поднимает три
// рабочих процесса разом, Проводник — свои, плюс соседние машины.
constexpr size_t kClientCount = 8;
constexpr size_t kAsyncIoBaseSlotSize = kSmbAdvertisedReadSize;
// 512-КиБ WRITE подтверждается только после физического завершения. Это не
// оставляет многоминутный write-back хвост перед FLUSH/CLOSE и связывает шаг
// стандартного клиентского индикатора с реально завершённой порцией Windows.
// Готовый PSRAM-снимок можно сериализовать пакетом: физических обращений к
// Z80 здесь нет. Холодный каталог принципиально другой — каждая запись требует
// отдельного WC_FINDNEXT через UART. Связанный запрос Проводника содержит два
// QUERY_DIRECTORY, поэтому пакет из 16 записей задерживал единый compound-ответ
// до 32 физических операций. MS-SMB2 задаёт OutputBufferLength как максимум,
// а не минимальное заполнение; возвращаем одну физическую запись и продолжаем
// тем же курсором в следующем запросе.
constexpr size_t kDirectoryBatchCapacity = 16;
constexpr size_t kDirectoryStreamingEntriesPerReply = 1;

uint32_t longIoInterimPendingMs() {
#ifdef ZIFI_HOST_BUILD
  // Отдельная host-регрессия сжимает 30 секунд до миллисекунд, не меняя
  // производственный контракт прошивки.
  const char* const text = getenv("ZIFI_HOST_INTERIM_PENDING_MS");
  if (text != nullptr && text[0] != 0) {
    char* end = nullptr;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end != text && *end == 0 && parsed > 0 && parsed <= 60000) {
      return static_cast<uint32_t>(parsed);
    }
  }
#endif
  return kLongIoInterimPendingMs;
}
// QUERY_DIRECTORY остаётся синхронным на проводе, но медленный FILEX исполняет
// вне callback SMB. Глубина совпадает с кредитным окном: память фиксирована, а
// второй QUERY_DIRECTORY из Windows compound всегда помещается в очередь.
constexpr size_t kAsyncDirectoryQueueDepth = 16;
// CREATE каталога остаётся синхронным на проводе, но при занятом FILEX
// переводится в ту же ограниченную очередь физических операций. Глубины
// кредитного окна достаточно: каждый ожидающий CREATE удерживает свой credit.
constexpr size_t kAsyncCreateQueueDepth = 16;
// CLOSE удерживает один из восьми handle и потому не может накопиться глубже
// таблицы handle. Отдельная очередь сохраняет общий порядок с WRITE, который
// в этот момент ещё может владеть FILEX.
constexpr size_t kAsyncCloseQueueDepth = kHandleCount;
// CHANGE_NOTIFY не занимает UART/PSRAM: это только ожидающий SMB Request и
// короткое имя. Глубина совпадает с числом одновременно учитываемых клиентов.
constexpr size_t kChangeNotifyDepth = kClientCount;
constexpr uint32_t kFileIdMagic = 0x424D535AUL;  // "ZSMB" в little-endian.
constexpr uint64_t kDirectoryFileIdOffset = 1469598103934665603ULL;
constexpr uint64_t kDirectoryFileIdPrime = 1099511628211ULL;
constexpr size_t kCreateContextHeaderSize = 16;
constexpr size_t kCreateContextNameOffset = 16;
constexpr size_t kCreateContextDataOffset = 24;
constexpr size_t kMaxCreateContextReplySize = 32 + 56;
constexpr uint32_t kLockFlagShared = 0x00000001;
constexpr uint32_t kLockFlagExclusive = 0x00000002;
constexpr uint32_t kLockFlagUnlock = 0x00000004;
constexpr uint32_t kLockFlagFailImmediately = 0x00000010;
constexpr uint32_t kStatusNotifyEnumDir = 0x0000010C;

struct RequestedCreateContexts {
  bool maximalAccess = false;
  bool maximalAccessHasTimestamp = false;
  bool queryOnDiskId = false;
  bool durableReconnect = false;
  uint64_t maximalAccessTimestamp = 0;
};

uint64_t appendDirectoryFileId(uint64_t hash, const char* text) {
  while (text != nullptr && *text != 0) {
    uint8_t value = static_cast<uint8_t>(*text++);
    // FAT и сам SMB-сервер сравнивают имена без учёта регистра ASCII. FileId
    // тоже должен описывать объект, а не конкретное написание пути в запросе.
    if (value >= 'A' && value <= 'Z') {
      value = static_cast<uint8_t>(value + ('a' - 'A'));
    }
    hash ^= value;
    hash *= kDirectoryFileIdPrime;
  }
  return hash;
}

// SessionId, TreeId и поколения FileId принадлежат одному запуску сервера и
// не должны повторять пространство уже разрушенного экземпляра.
uint32_t randomNonzero32() {
  uint32_t value = esp_random();
  if (value == 0 || value == 0xDEADBEEFUL) {
    value ^= 0x5A580001UL;
  }
  return value == 0 ? 1 : value;
}

uint64_t randomSessionSeed() {
  // Старший бит очищаем, чтобы даже при практически невозможном количестве
  // последовательных SESSION_SETUP счётчик не завернулся в ноль.
  uint64_t value = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
  value &= 0x7FFFFFFFFFFFFFFFULL;
  return value == 0 ? 1 : value;
}

void makeStableServerGuid(const char* netbiosName, uint8_t output[16]) {
  memset(output, 0, 16);
  if (netbiosName == nullptr) {
    return;
  }
  // Samba smbd кладёт NetBIOS-имя в 16-байтовый nstring и использует те же
  // байты как ServerGuid. Windows поэтому узнаёт один сервер и по имени, и по
  // IP после перезапуска listener; случайный GUID разрывал эту идентичность.
  for (size_t index = 0; index < 15 && netbiosName[index] != 0; ++index) {
    uint8_t value = static_cast<uint8_t>(netbiosName[index]);
    if (value >= 'A' && value <= 'Z') {
      value = static_cast<uint8_t>(value + ('a' - 'A'));
    }
    output[index] = value;
  }
}

void makeStableDiscoveryId(uint8_t output[16]) {
  // WSD отвечает на другой вопрос: это идентификатор физического устройства
  // для сетевой плитки Windows. Он стабилен по MAC, а SMB ServerGuid — по
  // NetBIOS-имени, как в Samba.
  memcpy(output, "ZiFiSMB!", 8);
  const uint64_t mac = ESP.getEfuseMac();
  for (size_t index = 0; index < 8; ++index) {
    output[8 + index] = static_cast<uint8_t>(mac >> (index * 8));
  }
}

// Коды CreateAction из MS-SMB2 2.2.14. Нумерация начинается с нуля, и раньше
// весь набор был сдвинут на единицу: на обычное открытие существующего файла
// сервер отвечал «FILE_CREATED». Копировщик Проводника считал, что источник
// только что создан, и замирал на нуле процентов, хотя данные по сети уже
// приходили. В дампе это видно прямо в ответе CREATE: запрос с
// CreateDisposition = FILE_OPEN, ответ с CreateAction = 2.
constexpr uint32_t kCreateSuperseded = 0;
constexpr uint32_t kCreateOpened = 1;
constexpr uint32_t kCreateCreated = 2;
constexpr uint32_t kCreateOverwritten = 3;

size_t minimum(size_t left, size_t right) {
  return left < right ? left : right;
}

uint64_t minimum64(uint64_t left, uint64_t right) {
  return left < right ? left : right;
}

uint32_t rpcFailureStatus(int result) {
  switch (result) {
    case -ENOMEM:
      return SMB2_STATUS_INSUFFICIENT_RESOURCES;
    case -ENOSPC:
      return SMB2_STATUS_BUFFER_TOO_SMALL;
    case -EINVAL:
      return SMB2_STATUS_INVALID_PARAMETER;
    case -ENOTSUP:
      return SMB2_STATUS_NOT_SUPPORTED;
    default:
      return SMB2_STATUS_INTERNAL_ERROR;
  }
}

void logRpcExchange(const char* transport,
                    const smb2_dcerpc_request_info& info,
                    int result, size_t responseLength) {
  diagnosticLogEvent(
      "SRVSVC rpc via=%s req=%u call=%lu context=%u accepted=%u opnum=%u "
      "resp=%u fault=%08lx result=%d bytes=%lu",
      transport, static_cast<unsigned>(info.pdu_type),
      static_cast<unsigned long>(info.call_id),
      static_cast<unsigned>(info.context_id),
      static_cast<unsigned>(info.accepted_context),
      static_cast<unsigned>(info.opnum),
      static_cast<unsigned>(info.response_pdu_type),
      static_cast<unsigned long>(info.fault_status), result,
      static_cast<unsigned long>(responseLength));
}

size_t padTo8(size_t value) {
  return (value + 7U) & ~static_cast<size_t>(7U);
}

size_t directoryFixedPart(uint8_t informationClass) {
  switch (informationClass) {
    case SMB2_FILE_FULL_DIRECTORY_INFORMATION:
      return SMB2_FILE_FULL_DIRECTORY_INFORMATION_SIZE;
    case SMB2_FILE_BOTH_DIRECTORY_INFORMATION:
      return SMB2_FILE_BOTH_DIRECTORY_INFORMATION_SIZE;
    case SMB2_FILE_ID_FULL_DIRECTORY_INFORMATION:
      return SMB2_FILEID_FULL_DIRECTORY_INFORMATION_SIZE;
    case SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION:
      return SMB2_FILEID_BOTH_DIRECTORY_INFORMATION_SIZE;
    default:
      return 0;
  }
}

size_t directoryEncodedSize(uint8_t informationClass, const char* name) {
  const size_t fixed = directoryFixedPart(informationClass);
  if (fixed == 0 || name == nullptr) {
    return 0;
  }
  // strlen()*2 заведомо не меньше реальной длины UTF-16: для ASCII она
  // точна, а для многобайтового UTF-8 даёт безопасную верхнюю границу.
  return padTo8(fixed + strlen(name) * 2U);
}

uint64_t roundUpAllocationSize(uint32_t size, uint32_t allocationUnit) {
  if (size == 0 || allocationUnit == 0) {
    return 0;
  }
  const uint64_t units =
      (static_cast<uint64_t>(size) + allocationUnit - 1U) / allocationUnit;
  return units * allocationUnit;
}

void setExternalError(char* output, size_t capacity, const char* text) {
  if (output != nullptr && capacity != 0) {
    snprintf(output, capacity, "%s", text == nullptr ? "" : text);
  }
}

bool asciiEqualNoCase(const char* left, const char* right) {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  while (*left != 0 && *right != 0) {
    const unsigned char a = static_cast<unsigned char>(*left++);
    const unsigned char b = static_cast<unsigned char>(*right++);
    if (toupper(a) != toupper(b)) {
      return false;
    }
  }
  return *left == 0 && *right == 0;
}

bool asciiPathBelongsTo(const char* value, const char* root) {
  if (value == nullptr || root == nullptr) {
    return false;
  }
  if (root[0] == '/' && root[1] == 0) {
    return value[0] == '/';
  }
  size_t index = 0;
  while (root[index] != 0 && value[index] != 0) {
    const unsigned char a = static_cast<unsigned char>(value[index]);
    const unsigned char b = static_cast<unsigned char>(root[index]);
    if (toupper(a) != toupper(b)) {
      return false;
    }
    ++index;
  }
  return root[index] == 0 &&
         (value[index] == 0 || value[index] == '/');
}

bool copyPayloadString(const uint8_t* payload, size_t length, size_t& offset,
                       char* output, size_t capacity) {
  if (payload == nullptr || output == nullptr || capacity == 0 ||
      offset > length) {
    return false;
  }
  size_t end = offset;
  while (end < length && payload[end] != 0) {
    ++end;
  }
  if (end == length || end - offset >= capacity) {
    return false;
  }
  memcpy(output, payload + offset, end - offset);
  output[end - offset] = 0;
  offset = end + 1;
  return true;
}

uint16_t readBe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0] << 8) | data[1];
}

void writeBe16(uint8_t* data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value >> 8);
  data[1] = static_cast<uint8_t>(value);
}

void writeBe32(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value >> 24);
  data[1] = static_cast<uint8_t>(value >> 16);
  data[2] = static_cast<uint8_t>(value >> 8);
  data[3] = static_cast<uint8_t>(value);
}

uint64_t readLe64Local(const uint8_t* data) {
  return static_cast<uint64_t>(readLe32(data)) |
         (static_cast<uint64_t>(readLe32(data + 4)) << 32);
}

void writeLe64Local(uint8_t* data, uint64_t value) {
  writeLe32(data, static_cast<uint32_t>(value));
  writeLe32(data + 4, static_cast<uint32_t>(value >> 32));
}

bool createContextNameEquals(const uint8_t* context, size_t contextLength,
                             uint16_t nameOffset, uint16_t nameLength,
                             const char expected[5]) {
  return nameLength == 4 && nameOffset <= contextLength &&
         static_cast<size_t>(nameLength) <= contextLength - nameOffset &&
         memcmp(context + nameOffset, expected, 4) == 0;
}

bool parseRequestedCreateContexts(const smb2_create_request& request,
                                  RequestedCreateContexts& parsed) {
  parsed = {};
  if (request.create_context_length == 0) {
    return true;
  }
  if (request.create_context == nullptr) {
    return false;
  }

  size_t offset = 0;
  const size_t totalLength = request.create_context_length;
  while (offset < totalLength) {
    const size_t remaining = totalLength - offset;
    if (remaining < kCreateContextHeaderSize) {
      return false;
    }
    const uint8_t* context = request.create_context + offset;
    const uint32_t next = readLe32(context);
    const uint16_t nameOffset = readLe16(context + 4);
    const uint16_t nameLength = readLe16(context + 6);
    const uint16_t dataOffset = readLe16(context + 10);
    const uint32_t dataLength = readLe32(context + 12);
    const size_t contextLength = next == 0 ? remaining : next;

    if (contextLength < kCreateContextHeaderSize || contextLength > remaining ||
        nameOffset < kCreateContextHeaderSize || (nameOffset & 7U) != 0 ||
        nameOffset > contextLength ||
        static_cast<size_t>(nameLength) > contextLength - nameOffset ||
        (dataLength != 0 &&
         (dataOffset < kCreateContextHeaderSize || (dataOffset & 7U) != 0 ||
          dataOffset > contextLength ||
          static_cast<size_t>(dataLength) > contextLength - dataOffset)) ||
        (next != 0 && (next & 7U) != 0)) {
      return false;
    }

    if (createContextNameEquals(context, contextLength, nameOffset, nameLength,
                                "MxAc")) {
      if (dataLength != 0 && dataLength != sizeof(uint64_t)) {
        return false;
      }
      parsed.maximalAccess = true;
      parsed.maximalAccessHasTimestamp = dataLength == sizeof(uint64_t);
      if (parsed.maximalAccessHasTimestamp) {
        parsed.maximalAccessTimestamp = readLe64Local(context + dataOffset);
      }
    } else if (createContextNameEquals(context, contextLength, nameOffset,
                                       nameLength, "QFid")) {
      if (dataLength != 0) {
        return false;
      }
      parsed.queryOnDiskId = true;
    } else if (createContextNameEquals(context, contextLength, nameOffset,
                                       nameLength, "DHnC") ||
               createContextNameEquals(context, contextLength, nameOffset,
                                       nameLength, "DH2C")) {
      parsed.durableReconnect = true;
    }

    if (next == 0) {
      break;
    }
    offset += next;
  }
  return true;
}

size_t appendCreateResponseContext(uint8_t* output, size_t capacity,
                                   size_t offset, const char name[5],
                                   const uint8_t* data, size_t dataLength) {
  const size_t contextLength = padTo8(kCreateContextDataOffset + dataLength);
  if (offset > capacity || contextLength > capacity - offset) {
    return 0;
  }
  uint8_t* context = output + offset;
  memset(context, 0, contextLength);
  writeLe16(context + 4, kCreateContextNameOffset);
  writeLe16(context + 6, 4);
  writeLe16(context + 10, kCreateContextDataOffset);
  writeLe32(context + 12, static_cast<uint32_t>(dataLength));
  memcpy(context + kCreateContextNameOffset, name, 4);
  if (dataLength != 0) {
    memcpy(context + kCreateContextDataOffset, data, dataLength);
  }
  return contextLength;
}

bool isCompoundFileId(const uint8_t id[SMB2_FD_SIZE]) {
  for (size_t index = 0; index < SMB2_FD_SIZE; ++index) {
    if (id[index] != 0xFF) {
      return false;
    }
  }
  return true;
}

bool wildcardMatch(const char* pattern, const char* text) {
  // Для обычного проводника достаточно DOS-масок '*' и '?'. Сравнение
  // латинских букв регистронезависимое, остальные UTF-8 байты сохраняются.
  const char* star = nullptr;
  const char* retry = nullptr;
  while (*text != 0) {
    if (*pattern == '?' ||
        (*pattern != 0 &&
         toupper(static_cast<unsigned char>(*pattern)) ==
             toupper(static_cast<unsigned char>(*text)))) {
      ++pattern;
      ++text;
    } else if (*pattern == '*') {
      star = pattern++;
      retry = text;
    } else if (star != nullptr) {
      pattern = star + 1;
      text = ++retry;
    } else {
      return false;
    }
  }
  while (*pattern == '*') {
    ++pattern;
  }
  return *pattern == 0;
}

uint32_t smbStatusFromFilex(uint8_t status) {
  switch (status) {
    case 0x00:
    case 0x25:  // SET_EOF committed; only post-commit cleanup failed.
      return SMB2_STATUS_SUCCESS;
    case 0x01:
      return SMB2_STATUS_END_OF_FILE;
    case 0x13:
    case 0x14:
    case 0x1C:
    case 0x1D:
    case 0x23:
      return SMB2_STATUS_INVALID_PARAMETER;
    case 0x15:
      return SMB2_STATUS_NOT_SUPPORTED;
    case 0x18:
      return SMB2_STATUS_MEDIA_WRITE_PROTECTED;
    case 0x19:
      return SMB2_STATUS_OBJECT_NAME_NOT_FOUND;
    case 0x1A:
      return SMB2_STATUS_OBJECT_NAME_COLLISION;
    case 0x1B:
      return SMB2_STATUS_DIRECTORY_NOT_EMPTY;
    case 0x22:
      return SMB2_STATUS_DISK_FULL;
    default:
      return SMB2_STATUS_IO_DEVICE_ERROR;
  }
}

bool fileTimeToFat(uint64_t value, uint16_t& date, uint16_t& timeValue,
                   uint8_t* createTenth = nullptr) {
  constexpr uint64_t kTicksPerSecond = 10000000ULL;
  constexpr uint64_t kUnixEpochSeconds = 11644473600ULL;
  if (value == 0 || value == UINT64_MAX) {
    return false;
  }
  const uint64_t wholeSeconds = value / kTicksPerSecond;
  if (wholeSeconds < kUnixEpochSeconds) {
    return false;
  }
  const time_t unixSeconds =
      static_cast<time_t>(wholeSeconds - kUnixEpochSeconds);
  struct tm parts = {};
  if (gmtime_r(&unixSeconds, &parts) == nullptr || parts.tm_year < 80 ||
      parts.tm_year > 207) {
    return false;
  }
  date = static_cast<uint16_t>(((parts.tm_year - 80) << 9) |
                               ((parts.tm_mon + 1) << 5) | parts.tm_mday);
  timeValue = static_cast<uint16_t>((parts.tm_hour << 11) |
                                    (parts.tm_min << 5) |
                                    (parts.tm_sec / 2));
  if (createTenth != nullptr) {
    const uint64_t subSecond = value % kTicksPerSecond;
    *createTenth = static_cast<uint8_t>((parts.tm_sec & 1) * 100 +
                                        subSecond / 100000ULL);
  }
  return true;
}

}  // namespace

struct SmbServer::Impl {
  enum class ActiveMode : uint8_t { kNone, kRead, kWrite, kDirectory };
  enum class CacheLoadResult : uint8_t { kReady, kUnavailable, kIoError };
  enum class DirectoryContents : uint8_t { kEmpty, kNotEmpty, kIoError };
  enum class TransferProgressMode : uint8_t { kNone, kRead, kWrite };

  struct TransferRange {
    uint32_t start = 0;
    uint32_t end = 0;
  };

  struct Handle {
    bool used = false;
    // Соединение, которому принадлежит дескриптор. При разрыве освобождаются
    // только его дескрипторы, чужие остаются открытыми.
    smb2_context* owner = nullptr;
    // [MS-SMB2] связывает каждый открытый объект не только с Session, но и с
    // TreeConnect. TREE_DISCONNECT обязан закрыть только объекты этого дерева.
    uint32_t treeId = 0;
    bool directory = false;
    bool pipe = false;
    bool writable = false;
    bool metadataDirty = false;
    bool metadataPending = false;
    bool deletePending = false;
    bool createdNew = false;
    bool failed = false;
    // Все Open одного пути видят логический EOF, заранее заданный SET_INFO,
    // но физически материализовать недостающий хвост имеет право только тот
    // Open, которому принадлежал SET_INFO. Иначе read-only наблюдатель
    // Проводника пытается сделать SET_EOF на CLOSE посреди чужого WRITE.
    bool sizeReserved = false;
    bool ownsSizeReservation = false;
    uint32_t generation = 0;
    uint32_t physicalSize = 0;
    // Длина файла на момент открытия. Разницу с итоговой сообщаем владельцу
    // счётчика свободного места один раз, на CLOSE: делать это на каждом
    // блоке нельзя — синхронный обмен с мостом посреди приёма данных
    // блокирует SMB-задачу и Windows теряет сессию.
    uint32_t openedSize = 0;
    uint32_t reservedSize = 0;
    uint32_t position = 0;
    // SMB-клиент вправе присылать READ/WRITE не по порядку смещений. position
    // остаётся файловой позицией последнего запроса, а индикатор отдельно
    // хранит объединение уже переданных диапазонов и потому не откатывается.
    TransferProgressMode progressMode = TransferProgressMode::kNone;
    uint32_t progressBytes = 0;
    size_t progressRangeCount = 0;
    size_t progressRangeCapacity = 0;
    TransferRange* progressRanges = nullptr;
    uint32_t directoryIndex = 0;
    bool directoryEnded = false;
    bool directoryCacheUnavailable = false;
    // Потоковый VFS уже сдвинул физическую позицию, когда следующая запись не
    // поместилась в остаток SMB-буфера. Сохраняем её в открытом объекте и
    // выдаём первой в следующем QUERY_DIRECTORY, не теряя имя и не перечитывая
    // каталог.
    bool directoryPending = false;
    bool directoryPendingIsDirectory = false;
    uint32_t directoryPendingSize = 0;
    uint32_t directoryPendingIndex = 0;
    char directoryPendingName[kMaxPath + 1] = {};
    // Холодный каталог возвращается по одной записи. Сразу после ответа
    // Проводник открывает показанный объект, одновременно уже поставив
    // следующий QUERY_DIRECTORY. В этот момент FILEX занят FINDNEXT, поэтому
    // сведения о последней выданной записи должны оставаться доступны без
    // нового STAT и без ожидания единственного физического канала.
    bool directoryLastValid = false;
    bool directoryLastIsDirectory = false;
    uint32_t directoryLastSize = 0;
    char directoryLastName[kMaxPath + 1] = {};
    DirectoryCache::Cursor directoryCursor{};
    uint16_t rpcContextId = 0xFFFF;
    uint32_t pipeResponseLength = 0;
    uint32_t pipeResponseOffset = 0;
    VfsMetadata pendingMetadata{};
    // Ответ принадлежит конкретному открытому экземпляру srvsvc. Общий буфер
    // смешивал WRITE/READ и PIPE_TRANSCEIVE разных дескрипторов.
    uint8_t pipeResponse[kSrvsvcResponseCapacity] = {};
    uint8_t fileId[SMB2_FD_SIZE] = {};
    char path[kMaxPath + 1] = {};
    char pattern[kMaxPath + 1] = "*";
  };

  struct ReportedMetadata {
    uint64_t creationTime = 0;
    uint64_t lastAccessTime = 0;
    uint64_t lastWriteTime = 0;
    uint64_t changeTime = 0;
    uint32_t attributes = 0;
  };

  struct CachedMetadata {
    bool used = false;
    uint32_t lastUse = 0;
    ReportedMetadata value{};
    char path[kMaxPath + 1] = {};
  };

  struct ByteRangeLock {
    bool used = false;
    bool exclusive = false;
    uint16_t ownerSlot = 0;
    uint32_t ownerGeneration = 0;
    uint64_t fileKey = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
  };

  struct Tree {
    bool used = false;
    bool ipc = false;
    uint32_t id = 0;
    smb2_context* owner = nullptr;
  };

  // Payload уже скопирован в соответствующий фиксированный PSRAM-слот.
  // Метаданные остаются до финального ответа на исходный async SMB-запрос.
  struct AsyncWrite {
    bool used = false;
    bool inFlight = false;
    bool cancelRequested = false;
    bool replied = false;
    bool pendingSent = false;
    bool writeThrough = false;
    smb2_context* context = nullptr;
    // Стабильный идентификатор владельца нужен после разрушения context. Сам
    // указатель больше не разыменовывается, а только сопоставляется с handle.
    smb2_context* owner = nullptr;
    uint64_t messageId = 0;
    uint64_t sequence = 0;
    int slot = -1;
    uint32_t generation = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t flushed = 0;
    uint32_t windowLength = 0;
    uint32_t requestStartedMs = 0;
    uint32_t lastProgressMs = 0;
  };

  // READ накапливает до 64 КиБ из нескольких физических VFS-окон. inFlight
  // означает, что одно окно прямо сейчас принадлежит core 1.
  struct AsyncRead {
    bool used = false;
    bool inFlight = false;
    bool cancelRequested = false;
    bool pendingSent = false;
    smb2_context* context = nullptr;
    smb2_context* owner = nullptr;
    uint64_t messageId = 0;
    uint64_t sequence = 0;
    int slot = -1;
    uint32_t generation = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t filled = 0;
    uint32_t windowLength = 0;
    uint32_t requestStartedMs = 0;
    uint32_t lastProgressMs = 0;
  };

  // Запрос сохранён внутри сервера без рабочего буфера. До 30 секунд исходный
  // PDU удерживает credit; затем может получить ровно один STATUS_PENDING и
  // AsyncId. Когда предыдущий READ завершится, запрос получает рабочий буфер.
  // Число дескрипторов жёстко ограничено кредитным окном, а не размером файла.
  struct QueuedRead {
    QueuedRead* next = nullptr;
    smb2_context* context = nullptr;
    bool pendingSent = false;
    uint64_t messageId = 0;
    uint64_t sequence = 0;
    int slot = -1;
    uint32_t generation = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t requestStartedMs = 0;
  };

  enum class AsyncDirectoryPhase : uint8_t {
    kPrepare,
    kCloseCommit,
    kCloseAbort,
    kOpen,
    kReplay,
    kRead,
  };

  // Маленькое описание отложенного QUERY_DIRECTORY. Данные каталога сюда не
  // копируются: один результат FILEX кодируется сразу после возврата моста.
  struct AsyncDirectory {
    bool used = false;
    bool prepared = false;
    bool inFlight = false;
    bool replied = false;
    bool cancelRequested = false;
    bool closeFailed = false;
    smb2_context* context = nullptr;
    uint64_t messageId = 0;
    uint64_t sequence = 0;
    int slot = -1;
    uint32_t generation = 0;
    uint32_t outputBufferLength = 0;
    uint32_t replayRemaining = 0;
    uint32_t lastProgressMs = 0;
    int previousSlot = -1;
    ActiveMode previousMode = ActiveMode::kNone;
    uint8_t informationClass = 0;
    uint8_t flags = 0;
    bool hasName = false;
    AsyncDirectoryPhase phase = AsyncDirectoryPhase::kPrepare;
    char name[kMaxPath + 1] = {};
  };

  enum class AsyncCreatePhase : uint8_t {
    kStat,
    kPrepare,
    kCloseCommit,
    kCloseAbort,
    kMkdir,
  };

  // Новый каталог нельзя создавать прямо из SMB callback, если единственный
  // FILEX-канал занят FINDNEXT/READ/WRITE. Сохраняем только поля, необходимые
  // для финального CREATE Response; имя из входного PDU сюда копируется.
  struct AsyncCreate {
    bool used = false;
    bool inFlight = false;
    bool cancelRequested = false;
    smb2_context* context = nullptr;
    smb2_context* owner = nullptr;
    uint64_t messageId = 0;
    uint64_t sequence = 0;
    uint64_t volumeId = 0;
    uint32_t treeId = 0;
    uint32_t desiredAccess = 0;
    uint32_t createOptions = 0;
    uint32_t lastProgressMs = 0;
    int previousSlot = -1;
    ActiveMode previousMode = ActiveMode::kNone;
    RequestedCreateContexts requestedContexts{};
    AsyncCreatePhase phase = AsyncCreatePhase::kPrepare;
    char path[kMaxPath + 1] = {};
  };

  struct AsyncClose {
    bool used = false;
    bool cancelRequested = false;
    smb2_context* context = nullptr;
    smb2_context* owner = nullptr;
    uint64_t messageId = 0;
    uint64_t sequence = 0;
    uint32_t generation = 0;
    uint16_t flags = 0;
    int slot = -1;
  };

  // Один ожидающий CHANGE_NOTIFY закреплён за открытым каталогом. После
  // STATUS_PENDING исходный Request PDU остаётся в waitqueue libsmb2, поэтому
  // для финала достаточно сохранить MessageId и проверить поколение handle.
  struct PendingNotify {
    bool used = false;
    smb2_context* context = nullptr;
    uint64_t messageId = 0;
    int slot = -1;
    uint32_t generation = 0;
    uint32_t completionFilter = 0;
    uint32_t outputBufferLength = 0;
    bool watchTree = false;
    char path[kMaxPath + 1] = {};
  };

  Impl(VfsBridge& bridgeValue, EventSink sink, void* sinkContext)
      : bridge(bridgeValue),
        eventSink(sink),
        eventContext(sinkContext),
        udp(),
        udpLlmnr(),
        wsDiscovery(),
        server{},
        discoveryId{},
        handlers{},
        task(nullptr),
        taskAlive(false),
        runningFlag(false),
        discoveryRunning(false),
        llmnrRunning(false),
        lastServeResult(0),
        clients{},
        trees{},
        nextTreeId(kFirstTreeId),
        handles{},
        metadataCache{},
        byteRangeLocks{},
        metadataUseCounter(0),
        generationCounter(1),
        activeSlot(-1),
        activeMode(ActiveMode::kNone),
        activeLogicalOffset(0),
        activeVfsOffset(0),
        activeRandomWrite(false),
        asyncReads{},
        asyncWrites{},
        asyncIoBuffers{},
        asyncIoBufferCapacities{},
        activeAsyncRead(-1),
        activeAsyncWrite(-1),
        asyncReadCount(0),
        asyncWriteCount(0),
        queuedReadHead(nullptr),
        queuedReadTail(nullptr),
        queuedReadCount(0),
        asyncDirectories{},
        activeAsyncDirectory(-1),
        asyncDirectoryCount(0),
        asyncCreates{},
        activeAsyncCreate(-1),
        asyncCreateCount(0),
        asyncCloses{},
        activeAsyncClose(-1),
        asyncCloseCount(0),
        asyncVfsSequence(0),
        pendingNotifies{},
        portValue(kDefaultPort),
        share{},
        hostname{},
        workgroup{},
        user{},
        password{},
        lastVfsError{},
        volumeLabel{},
        lastOperationText{},
        lastProgressText{},
        progressStampMs(0),
        directoryBatch(nullptr),
        directoryBatchNames(nullptr),
        directoryInfo{},
        directoryName{},
        infoName{},
        basicInfo{},
        standardInfo{},
        allInfo{},
        internalInfo{},
        networkInfo{},
        nameInfo{},
        positionInfo{},
        streamInfo{},
        directoryCache(),
        directoryCacheBuildSlot(-1),
        directoryCacheBuildGeneration(0),
        cachedFilePath{},
        cachedFileData(nullptr),
        cachedFileSize(0),
        cachedFileFilled(0),
        activeRandomRead(false),
        cachedFsInfo{},
        fsInfoValid(false),
        fsInfoRefreshRequired(true),
        allocationUnitBytes(0),
        volumeInfo{},
        sizeInfo{},
        fullSizeInfo{},
        attributeInfo{},
        deviceInfo{},
        controlInfo{},
        sectorInfo{},
        createContextReply{},
        scratch{},
        nbnsPacket{} {
    snprintf(share, sizeof(share), "SD");
    snprintf(hostname, sizeof(hostname), "ZX-Evo");
    snprintf(workgroup, sizeof(workgroup), "WORKGROUP");
    snprintf(user, sizeof(user), "zx");
    snprintf(password, sizeof(password), "zx");
    snprintf(lastVfsError, sizeof(lastVfsError), "none");
    snprintf(volumeLabel, sizeof(volumeLabel), "ZX EVO SD");

    handlers.destruction_event = destructionHandler;
    handlers.service_event = serviceHandler;
    handlers.authorize_user = authorizeHandler;
    handlers.session_established = sessionHandler;
    handlers.logoff_cmd = logoffHandler;
    handlers.tree_connect_cmd = treeConnectHandler;
    handlers.tree_disconnect_cmd = treeDisconnectHandler;
    handlers.create_cmd = createHandler;
    handlers.close_cmd = closeHandler;
    handlers.flush_cmd = flushHandler;
    handlers.read_cmd = readHandler;
    handlers.write_cmd = writeHandler;
    handlers.oplock_break_cmd = oplockHandler;
    handlers.lease_break_cmd = leaseHandler;
    handlers.lock_cmd = lockHandler;
    handlers.ioctl_cmd = ioctlHandler;
    handlers.cancel_cmd = cancelHandler;
    handlers.echo_cmd = echoHandler;
    handlers.query_directory_cmd = queryDirectoryHandler;
    handlers.change_notify_cmd = changeNotifyHandler;
    handlers.query_info_cmd = queryInfoHandler;
    handlers.set_info_cmd = setInfoHandler;
  }

  ~Impl() {
    for (size_t index = 0; index < kHandleCount; ++index) {
      clearTransferProgress(handles[index], true);
    }
    releaseDirectoryBatch();
    releaseAsyncIoStorage();
  }

  VfsBridge& bridge;
  EventSink eventSink;
  void* eventContext;
  WiFiUDP udp;
  WiFiUDP udpLlmnr;
  WsDiscovery wsDiscovery;
  smb2_server server;
  uint8_t discoveryId[16];
  smb2_server_request_handlers handlers;
  TaskHandle_t task;
  volatile bool taskAlive;
  volatile bool runningFlag;
  bool discoveryRunning;
  bool llmnrRunning;
  volatile int lastServeResult;
  // Проводник открывает к серверу несколько TCP-соединений сразу: отдельно
  // копирование, отдельно потоки оболочки с эскизами и типами файлов. Раньше
  // новое соединение вытесняло предыдущее вместе с его открытыми файлами, и
  // копирование обрывалось на середине. Физический канал к Z80 по-прежнему
  // один — его делят те же activeSlot/closeActive, что делят открытые файлы
  // внутри одного соединения.
  smb2_context* clients[kClientCount];

  Tree trees[kTreeCount];
  uint32_t nextTreeId;
  Handle handles[kHandleCount];
  CachedMetadata metadataCache[kMetadataCacheCount];
  ByteRangeLock byteRangeLocks[kByteRangeLockCount];
  uint32_t metadataUseCounter;
  uint32_t generationCounter;
  int activeSlot;
  ActiveMode activeMode;
  uint32_t activeLogicalOffset;
  uint32_t activeVfsOffset;
  // SET_METADATA работает только поверх FILEX random mode=3. Этот флаг
  // защищает контракт активного WRITE-контекста от случайного возврата к
  // последовательному mode=1 с отложенным последним окном.
  bool activeRandomWrite;
  AsyncRead asyncReads[kAsyncIoQueueDepth];
  AsyncWrite asyncWrites[kAsyncIoQueueDepth];
  uint8_t* asyncIoBuffers[kAsyncIoQueueDepth];
  size_t asyncIoBufferCapacities[kAsyncIoQueueDepth];
  int activeAsyncRead;
  int activeAsyncWrite;
  size_t asyncReadCount;
  size_t asyncWriteCount;
  QueuedRead* queuedReadHead;
  QueuedRead* queuedReadTail;
  size_t queuedReadCount;
  AsyncDirectory asyncDirectories[kAsyncDirectoryQueueDepth];
  int activeAsyncDirectory;
  size_t asyncDirectoryCount;
  AsyncCreate asyncCreates[kAsyncCreateQueueDepth];
  int activeAsyncCreate;
  size_t asyncCreateCount;
  AsyncClose asyncCloses[kAsyncCloseQueueDepth];
  int activeAsyncClose;
  size_t asyncCloseCount;
  // Общая очередь физических операций. Разные SMB-сеансы могут прислать
  // READ и QUERY_DIRECTORY почти одновременно, но единственный FILEX-канал
  // обязан обслужить их в порядке поступления.
  uint64_t asyncVfsSequence;
  PendingNotify pendingNotifies[kChangeNotifyDepth];

  uint16_t portValue;
  char share[32];
  char hostname[16];
  char workgroup[16];
  char user[33];
  char password[65];
  char lastVfsError[64];
  char volumeLabel[12];
  // Последние отправленные строки индикации: одинаковый текст повторно по
  // UART не гоняется, а прогресс дополнительно ограничен по частоте.
  char lastOperationText[32];
  char lastProgressText[32];
  uint32_t progressStampMs;

  uint8_t* directoryBatch;
  char* directoryBatchNames;
  smb2_fileidbothdirectoryinformation directoryInfo;
  char directoryName[kMaxPath + 1];
  char infoName[kMaxPath + 2];
  smb2_file_basic_info basicInfo;
  smb2_file_standard_info standardInfo;
  smb2_file_ea_info eaInfo;
  smb2_file_all_info allInfo;
  smb2_file_internal_info internalInfo;
  smb2_file_network_open_info networkInfo;
  smb2_file_name_info nameInfo;
  smb2_file_position_info positionInfo;
  smb2_file_stream_info streamInfo;
  DirectoryCache directoryCache;
  // Один незавершённый снимок строится прямо из того же физического
  // перечисления, которым обслуживается первый SMB Open. Второго фонового
  // OPENDIR/FINDNEXT нет, поэтому запись не может попасть в ответ дважды.
  int directoryCacheBuildSlot;
  uint32_t directoryCacheBuildGeneration;
  // Копия целиком прочитанного файла в PSRAM. Заполняется только строго
  // последовательным чтением с нулевого смещения и живёт до первой правки
  // тома. Нужна из-за того, что Проводник читает исходный файл дважды.
  char cachedFilePath[kMaxPath + 1];
  uint8_t* cachedFileData;
  uint32_t cachedFileSize;
  uint32_t cachedFileFilled;
  // Режим, которым открыт текущий читаемый файл: позиционный тракт FILEX
  // ограничен окном 16352 байта, последовательный допускает полные 16 КиБ.
  bool activeRandomRead;
  // Сведения о томе. Кэш локальный и намеренно: любой обмен с мостом внутри
  // приёма данных блокирует SMB-задачу, и Windows теряет сессию.
  VfsFsInfo cachedFsInfo;
  bool fsInfoValid;
  // Геометрия FAT32 неизменна, пока плагин монопольно владеет картой. В
  // начале каждого запуска сбрасываем оба уровня кэша, затем один раз читаем
  // размер кластера до первого ответа CREATE. Изменения файлов инвалидируют
  // свободное место, но не эту величину.
  bool fsInfoRefreshRequired;
  uint32_t allocationUnitBytes;
  smb2_file_fs_volume_info volumeInfo;
  smb2_file_fs_size_info sizeInfo;
  smb2_file_fs_full_size_info fullSizeInfo;
  smb2_file_fs_attribute_info attributeInfo;
  smb2_file_fs_device_info deviceInfo;
  smb2_file_fs_control_info controlInfo;
  smb2_file_fs_sector_size_info sectorInfo;
  uint8_t createContextReply[kMaxCreateContextReplySize];

  uint8_t scratch[512];
  uint8_t nbnsPacket[576];

  // Адрес, на котором подняты сокеты обнаружения. Выдаётся по DHCP и способен
  // смениться сам, без нашего участия — это единственный параметр среды,
  // который не контролирует ни прошивка, ни пользователь.
  uint32_t announcedAddress = 0;

  bool start(const uint8_t* payload, uint16_t length, uint16_t& actualPort,
             bool& netbiosActive, char* error, size_t errorSize);
  bool stop();
  void pollDiscovery();
  void startDiscovery();
  void stopDiscovery();
  void answerNbns(size_t length);
  void answerLlmnr(const uint8_t* packet, size_t length);
  bool addressChanged();

  static void taskEntry(void* context);
  void taskLoop();
  static void newClient(smb2_context* smb2, void* context);
  static void libraryError(smb2_context* smb2, const char* text);

  void resetHandles();
  void releaseByteRangeLocksForHandle(int slot, uint32_t generation);
  bool fileIoConflictsWithLock(int slot, uint64_t offset, uint64_t length,
                               bool writeAccess) const;
  bool lockRangeConflicts(uint64_t fileKey, int ownerSlot,
                          uint32_t ownerGeneration, uint64_t offset,
                          uint64_t length, bool exclusive) const;
  bool allocateAsyncIoStorage();
  void releaseAsyncIoStorage();
  void resetAsyncIo();
  void clearQueuedReads();
  uint64_t nextAsyncVfsSequence();
  uint64_t oldestAsyncReadSequence() const;
  uint64_t oldestAsyncWriteSequence() const;
  uint64_t oldestAsyncDirectorySequence() const;
  uint64_t oldestAsyncCreateSequence() const;
  uint64_t oldestAsyncCloseSequence() const;
  int allocateAsyncRead();
  int allocateAsyncWrite(size_t requiredCapacity);
  bool enqueueAsyncRead(smb2_context* context, uint64_t messageId, int slot,
                        uint64_t sequence, uint32_t generation, uint32_t offset,
                        uint32_t length);
  bool beginAsyncRead(smb2_context* context, uint64_t messageId, int slot,
                      uint64_t sequence, uint32_t generation, uint32_t offset,
                      uint32_t length, uint32_t requestStartedMs,
                      bool pendingSent, int index);
  void promoteQueuedReads();
  void failQueuedReads(uint32_t status);
  void dropQueuedReadsForOwner(smb2_context* owner);
  bool cancelQueuedRead(smb2_context* owner, uint64_t messageId);
  int findReadyAsyncRead() const;
  int findReadyAsyncWrite() const;
  bool hasAsyncWritesForHandle(int slot, uint32_t generation) const;
  bool hasAsyncIoForOwner(smb2_context* owner) const;
  void releaseDetachedOwnerIfIdle(smb2_context* owner);
  bool drainAsyncWritesForHandle(int slot, uint32_t generation);
  bool asyncIoTimedOut() const;
  void discardAsyncReadData(int index);
  void completeAsyncReadWithStatus(int index, uint32_t status,
                                   bool closePhysical);
  void completeCancelledAsyncWrite(int index, bool closePhysical);
  void failAsyncReads(uint32_t status);
  void failAsyncWrites(uint32_t status);
  void releaseClientHandles(smb2_context* owner);
  bool releaseTreeHandles(smb2_context* owner, uint32_t treeId);
  bool addClient(smb2_context* smb2);
  void forgetClient(smb2_context* smb2);
  bool knownClient(smb2_context* smb2) const;
  size_t clientCount() const;
  bool allocateDirectoryBatch();
  void releaseDirectoryBatch();
  void cleanupClient(smb2_context* smb2);
  void dropNotifiesForOwner(smb2_context* owner);
  void cancelNotifiesForHandle(smb2_context* owner, int slot,
                               uint32_t generation, uint32_t status);
  bool cancelNotify(smb2_context* owner, uint64_t messageId);
  bool queueNotifyReply(PendingNotify& pending, uint32_t action,
                        const char* relativeName, uint32_t secondAction = 0,
                        const char* secondRelativeName = nullptr);
  bool makeNotifyRelative(const PendingNotify& pending, const char* path,
                          char relative[kMaxPath + 1]) const;
  void notifyChange(const char* path, uint32_t action, uint32_t filter);
  void notifyRename(const char* oldPath, const char* newPath,
                    uint32_t filter);
  void sendClientEvent(uint8_t state);
  void sendOperation(const char* operation, const char* path = nullptr);
  bool noteTransferProgress(Handle& handle, TransferProgressMode mode,
                            uint32_t offset, uint32_t length);
  void clipTransferProgress(Handle& handle, uint32_t size);
  void clearTransferProgress(Handle& handle, bool releaseMemory);
  // Ход текущей передачи. force обязателен на завершении, иначе последнее
  // обновление может не пройти ограничение частоты и счётчик замрёт до конца.
  void sendProgress(const Handle& handle, const char* operation,
                    uint32_t position, bool force);
  void resetProgress();
  bool requestVfs(VfsOperation operation, const char* path, uint32_t value,
                  VfsResult& result, uint32_t timeoutMs);
  bool requestVfsAt(VfsOperation operation, uint32_t offset, uint32_t length,
                    VfsResult& result, uint32_t timeoutMs);
  bool requestRename(const char* oldPath, const char* newName, bool directory,
                     VfsResult& result);
  bool requestMoveRename(const char* oldPath, const char* newPath,
                         bool directory, bool replace, VfsResult& result);
  bool requestMetadata(const VfsMetadata& metadata, VfsResult& result);
  bool closeActive(bool commit);
  bool resetBuffers();
  bool statPath(const char* path, VfsResult& result);
  bool createEmptyFile(const char* path);
  bool removePath(const char* path);
  CacheLoadResult ensureDirectoryCached(const char* path);
  DirectoryContents checkDirectoryContents(const char* path);
  bool beginDirectoryCacheBuild(int slot, Handle& handle);
  bool appendDirectoryCacheBuild(int slot, Handle& handle,
                                 const VfsResult& result);
  void finishDirectoryCacheBuild(int slot, Handle& handle);
  void abortDirectoryCacheBuild();
  void invalidateDirectory(const char* path);
  void invalidateSubtree(const char* path);
  void invalidateParent(const char* path);
  // Сведения о томе читаются через кэш: это единственная операция, которая
  // может заставить Wild Commander пересчитать всю FAT, а Проводник опрашивает
  // свободное место в течение всего копирования.
  bool loadFsInfo(VfsFsInfo& info, uint8_t& status);
  bool ensureAllocationUnit(uint8_t& status);
  uint64_t reportedAllocationSize(uint32_t physicalSize,
                                  bool directory) const;
  void invalidateFsInfo();
  void dropFileCache();
  void beginFileCache(const char* path, uint32_t size);
  void appendFileCache(uint32_t offset, const uint8_t* data, uint32_t length);
  bool serveFromFileCache(const char* path, uint32_t size, uint32_t offset,
                          uint32_t length, uint8_t* output) const;
  // Чистая арифметика над локальным кэшем: ни UART, ни моста.
  void noteFileGrowth(uint32_t oldSize, uint32_t newSize);
  // Один FAT-файл может быть открыт несколькими SMB handle (так делает
  // CopyFile). Физическая длина и выполненное резервирование у них общие.
  void updateSharedPhysicalSize(const char* path, uint32_t newSize);
  // Обновляет размер записи в снимке родителя без сброса всего снимка. Именно
  // отсюда Проводник берёт размер растущего файла во время копирования.
  void refreshCachedSize(const char* path, uint32_t size);
  bool activateRead(int slot, uint32_t offset);
  bool fetchReadWindow(Handle& handle);
  bool activateWrite(int slot, uint32_t offset);
  bool commitReservedSize(int slot);
  void deferMetadata(Handle& handle, const VfsMetadata& metadata);
  bool applyPendingMetadata(int slot);
  uint32_t finalizeClose(int slot, uint32_t generation, uint16_t flags,
                         smb2_close_reply& reply);
  bool activateDirectory(int slot);
  void pollAsyncRead();
  void pollAsyncWrite();
  void pollIoInterimPending();
  bool queueIoInterimPending(smb2_context* context, uint8_t command,
                             uint64_t messageId, bool& pendingSent);
  void restoreServerCreditsIfIoIdle(smb2_context* context);
  void pollAsyncDirectory();
  int allocateAsyncDirectory() const;
  bool enqueueAsyncDirectory(smb2_context* context, uint64_t messageId,
                             int slot, uint32_t generation,
                             const smb2_query_directory_request& request);
  bool queueAsyncDirectoryReply(const AsyncDirectory& pending,
                                Handle& handle, const char* name,
                                bool directory, uint32_t size,
                                uint32_t fileIndex);
  void completeAsyncDirectory(int index, uint32_t status);
  void failAsyncDirectories(uint32_t status);
  void dropAsyncDirectoriesForOwner(smb2_context* owner);
  bool cancelAsyncDirectory(smb2_context* owner, uint64_t messageId);
  int allocateAsyncCreate() const;
  bool enqueueAsyncCreate(smb2_context* context, uint64_t messageId,
                          uint32_t treeId, uint32_t desiredAccess,
                          uint32_t createOptions,
                          const RequestedCreateContexts& requestedContexts,
                          uint64_t volumeId, bool statRequired,
                          const char* path);
  bool queueAsyncCreateReply(AsyncCreate& pending);
  void completeAsyncCreate(int index, uint32_t status);
  void pollAsyncCreate();
  void failAsyncCreates(uint32_t status);
  void dropAsyncCreatesForOwner(smb2_context* owner);
  bool cancelAsyncCreate(smb2_context* owner, uint64_t messageId);
  int allocateAsyncClose() const;
  bool enqueueAsyncClose(smb2_context* context, uint64_t messageId, int slot,
                         uint32_t generation, uint16_t flags);
  void completeAsyncClose(int index, uint32_t status,
                          const smb2_close_reply* reply = nullptr);
  void pollAsyncClose();
  void dropAsyncClosesForOwner(smb2_context* owner);
  bool cancelAsyncClose(smb2_context* owner, uint64_t messageId);
  bool queueAsyncStatus(smb2_context* smb2, uint8_t command,
                        uint32_t status, uint64_t messageId);
  bool queueAsyncWriteReply(smb2_context* smb2, uint64_t messageId,
                            uint32_t count);
  bool queueAsyncReadReply(smb2_context* smb2, uint64_t messageId,
                           const uint8_t* data, uint32_t count);

  bool normalizePath(const char* input, char output[kMaxPath + 1]) const;
  bool splitParent(const char* path, char parent[kMaxPath + 1],
                   char name[kMaxPath + 1]) const;
  bool parseShare(const smb2_tree_connect_request* request,
                  char output[32]) const;
  bool makeInfoName(const Handle& handle);

  int allocateHandle(smb2_context* owner, uint32_t treeId);
  Handle* findHandle(smb2_context* owner,
                     const uint8_t fileId[SMB2_FD_SIZE],
                     int* slot = nullptr);
  void releaseHandle(int slot);
  uint32_t allocateTree(bool ipc, smb2_context* owner);
  const Tree* findTree(uint32_t id) const;
  void releaseTree(uint32_t id);
  uint32_t visibleSize(const Handle& handle) const;
  uint64_t directoryFileId(const char* path) const;
  uint64_t directoryChildFileId(const char* parentPath,
                                const char* name) const;
  ReportedMetadata reportedMetadata(const char* path, bool directory) const;
  ReportedMetadata reportedChildMetadata(const char* parentPath,
                                          const char* name,
                                          bool directory) const;
  void rememberMetadata(const char* path, bool directory,
                        const VfsMetadata& metadata,
                        uint8_t appliedAttributes);
  void forgetMetadata(const char* path);
  void renameMetadata(const char* oldPath, const char* newPath);
  void fillCreateContextReply(const RequestedCreateContexts& requested,
                              uint64_t diskFileId, uint64_t volumeId,
                              uint64_t changeTime, uint32_t maximalAccess,
                              smb2_create_reply& reply);
  uint64_t currentFileTime() const;
  smb2_timeval currentSmb2Time() const;
  void fillDirectoryInfo(smb2_fileidbothdirectoryinformation& info,
                         uint32_t index, bool directory, uint32_t size,
                         const char* parentPath, const char* name) const;
  int queryCachedDirectory(smb2_context* smb2, Handle& handle,
                           smb2_query_directory_request* request,
                           smb2_query_directory_reply* reply);
  int queryStreamedDirectory(smb2_context* smb2, Handle& handle, int slot,
                             smb2_query_directory_request* request,
                             smb2_query_directory_reply* reply);

  static Impl* from(smb2_server* serverValue) {
    return serverValue == nullptr
               ? nullptr
               : static_cast<Impl*>(serverValue->auth_data);
  }
  static int replyStatus(smb2_context* smb2, uint8_t command,
                         uint32_t status);
  static int replyIoctlStatus(smb2_context* smb2,
                              smb2_ioctl_reply* reply, uint32_t status);
  static int createStatus(smb2_context* smb2, smb2_create_request* request,
                          uint32_t status);

  static int destructionHandler(smb2_server*, smb2_context*);
  static int serviceHandler(smb2_server*);
  static int authorizeHandler(smb2_server*, smb2_context*, const char*,
                              const char*, const char*);
  static int sessionHandler(smb2_server*, smb2_context*);
  static int logoffHandler(smb2_server*, smb2_context*);
  static int treeConnectHandler(smb2_server*, smb2_context*,
                                smb2_tree_connect_request*,
                                smb2_tree_connect_reply*);
  static int treeDisconnectHandler(smb2_server*, smb2_context*, uint32_t);
  static int createHandler(smb2_server*, smb2_context*, smb2_create_request*,
                           smb2_create_reply*);
  static int closeHandler(smb2_server*, smb2_context*, smb2_close_request*,
                          smb2_close_reply*);
  static int flushHandler(smb2_server*, smb2_context*, smb2_flush_request*);
  static int readHandler(smb2_server*, smb2_context*, smb2_read_request*,
                         smb2_read_reply*);
  static int writeHandler(smb2_server*, smb2_context*, smb2_write_request*,
                          smb2_write_reply*);
  static int oplockHandler(smb2_server*, smb2_context*,
                           smb2_oplock_break_acknowledgement*);
  static int leaseHandler(smb2_server*, smb2_context*,
                          smb2_lease_break_acknowledgement*);
  static int lockHandler(smb2_server*, smb2_context*, smb2_lock_request*);
  static int ioctlHandler(smb2_server*, smb2_context*, smb2_ioctl_request*,
                          smb2_ioctl_reply*);
  static int cancelHandler(smb2_server*, smb2_context*);
  static int echoHandler(smb2_server*, smb2_context*);
  static int queryDirectoryHandler(smb2_server*, smb2_context*,
                                   smb2_query_directory_request*,
                                   smb2_query_directory_reply*);
  static int changeNotifyHandler(smb2_server*, smb2_context*,
                                 smb2_change_notify_request*,
                                 smb2_change_notify_reply*);
  static int queryInfoHandler(smb2_server*, smb2_context*,
                              smb2_query_info_request*, smb2_query_info_reply*);
  static int setInfoHandler(smb2_server*, smb2_context*,
                            smb2_set_info_request*);
};

bool SmbServer::Impl::start(const uint8_t* payload, uint16_t length,
                            uint16_t& actualPort, bool& netbiosActive,
                            char* error, size_t errorSize) {
  actualPort = 0;
  netbiosActive = false;
  diagnosticLogEvent("SMB start-request payload=%u task=%u",
                     static_cast<unsigned>(length), taskAlive ? 1U : 0U);
  if (taskAlive) {
    setExternalError(error, errorSize, "smb already running");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setExternalError(error, errorSize, "smb no wifi");
    return false;
  }
  if (!bridge.ready()) {
    setExternalError(error, errorSize, "smb vfs bridge");
    return false;
  }
  if (length != 0 && payload == nullptr) {
    setExternalError(error, errorSize, "smb payload null");
    return false;
  }

  portValue = length >= 2 ? readLe16(payload) : kDefaultPort;
  if (portValue == 0) {
    setExternalError(error, errorSize, "smb port zero");
    return false;
  }
  snprintf(share, sizeof(share), "SD");
  snprintf(hostname, sizeof(hostname), "ZX-Evo");
  snprintf(workgroup, sizeof(workgroup), "WORKGROUP");
  snprintf(user, sizeof(user), "zx");
  snprintf(password, sizeof(password), "zx");

  size_t offset = length >= 2 ? 2 : length;
  char* fields[] = {share, hostname, workgroup, user, password};
  const size_t capacities[] = {sizeof(share), sizeof(hostname),
                               sizeof(workgroup), sizeof(user),
                               sizeof(password)};
  for (size_t index = 0; offset < length && index < 5; ++index) {
    char parsed[65] = {};
    if (!copyPayloadString(payload, length, offset, parsed,
                           minimum(sizeof(parsed), capacities[index]))) {
      setExternalError(error, errorSize, "smb option too long");
      return false;
    }
    if (parsed[0] != 0) {
      snprintf(fields[index], capacities[index], "%s", parsed);
    }
  }
  if (offset != length || share[0] == 0 || hostname[0] == 0 ||
      workgroup[0] == 0 || user[0] == 0 || password[0] == 0 ||
      strchr(share, '\\') != nullptr || strchr(share, '/') != nullptr) {
    setExternalError(error, errorSize, "smb bad options");
    return false;
  }

  stopDiscovery();
  // Резервируем очередь до кэша каталога: их области независимы, а нехватка
  // памяти не может проявиться уже посередине принятого Windows READ/WRITE.
  if (!allocateAsyncIoStorage()) {
    setExternalError(error, errorSize, "smb io psram");
    return false;
  }
  // Плагин получает SD-карту монопольно. С этого момента каталог в PSRAM
  // является точной копией до известной серверу операции изменения.
  directoryCache.begin(psramFound());
  // Строки индикации относятся к прошлому сеансу.
  lastOperationText[0] = 0;
  lastProgressText[0] = 0;
  allocateDirectoryBatch();
  resetHandles();
  fsInfoValid = false;
  fsInfoRefreshRequired = true;
  allocationUnitBytes = 0;
  memset(&server, 0, sizeof(server));
  server.fd = -1;
  server.port = portValue;
  server.handlers = &handlers;
  server.auth_data = this;
  // По MS-SMB2 сервер обязан объявлять SIGNING_ENABLED даже при необязательной
  // подписи. В первом тестовом образе 0.6.86 здесь был ноль: сырой probe
  // принимал ответ, но Windows отклонял NEGOTIATE с системной ошибкой 8.
  // Объявляем поддержку signing, а
  // ниже не ставим SIGNING_REQUIRED: политика клиента решает, подписывать ли
  // сеанс, без изменения настроек Windows/FAR.
  server.signing_enabled = 1;
  server.allow_anonymous = 0;
  server.max_transact_size = kSmbAdvertisedTransactSize;
  server.max_read_size = kSmbAdvertisedReadSize;
  server.max_write_size = kSmbAdvertisedWriteSize;
  snprintf(server.hostname, sizeof(server.hostname), "%s", hostname);
  snprintf(server.domain, sizeof(server.domain), "%s", workgroup);

  makeStableServerGuid(hostname, server.guid);
  makeStableDiscoveryId(discoveryId);
  // libsmb2 увеличивает SessionId для последующих SESSION_SETUP; случайна
  // только начальная точка данного запуска. TreeId и generationCounter далее
  // также работают как обычные монотонные счётчики внутри этого экземпляра.
  server.session_counter = randomSessionSeed();
  nextTreeId = randomNonzero32();
  generationCounter = randomNonzero32();

  lastServeResult = 0;
  taskAlive = true;
  runningFlag = false;
  if (xTaskCreatePinnedToCore(taskEntry, "zifi-smb", kSmbTaskStackBytes, this,
                              kSmbTaskPriority, &task,
                              kSmbTaskCore) != pdPASS) {
    taskAlive = false;
    task = nullptr;
    directoryCache.clear();
    releaseDirectoryBatch();
    releaseAsyncIoStorage();
    diagnosticLogEvent("SMB task-create-failed");
    setExternalError(error, errorSize, "smb task create");
    return false;
  }

  const uint32_t started = millis();
  while (taskAlive && server.listener_ready == 0 &&
         static_cast<uint32_t>(millis() - started) < 5000) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  if (server.listener_ready == 0) {
    server.stop_requested = 1;
    const int result = lastServeResult;
    char message[64];
    snprintf(message, sizeof(message), "smb listen failed:%d", result);
    diagnosticLogEvent("SMB start-timeout result=%d", result);
    setExternalError(error, errorSize, message);
    return false;
  }

  runningFlag = true;
  startDiscovery();
  diagnosticLogEvent("SMB started port=%u fd=%d nbns=%u wsd=%u",
                     static_cast<unsigned>(portValue), server.fd,
                     discoveryRunning ? 1U : 0U,
                     wsDiscovery.running() ? 1U : 0U);
  actualPort = portValue;
  netbiosActive = discoveryRunning;
  setExternalError(error, errorSize, "");
  return true;
}

bool SmbServer::Impl::stop() {
  diagnosticLogEvent("SMB stop-request task=%u running=%u stop=%d",
                     taskAlive ? 1U : 0U, runningFlag ? 1U : 0U,
                     server.stop_requested);
  stopDiscovery();
  if (!taskAlive) {
    runningFlag = false;
    directoryCache.clear();
    releaseDirectoryBatch();
    releaseAsyncIoStorage();
    return true;
  }
  server.stop_requested = 1;
  const uint32_t started = millis();
  while (taskAlive &&
         static_cast<uint32_t>(millis() - started) < 3000) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  runningFlag = taskAlive;
  if (!taskAlive) {
    // После выхода из плагина Wild Commander снова может менять SD-карту,
    // поэтому старые снимки нельзя сохранять до следующего запуска.
    directoryCache.clear();
    releaseDirectoryBatch();
    releaseAsyncIoStorage();
  }
  diagnosticLogEvent("SMB stop-result task=%u", taskAlive ? 1U : 0U);
  return !taskAlive;
}

void SmbServer::Impl::taskEntry(void* context) {
  static_cast<Impl*>(context)->taskLoop();
}

void SmbServer::Impl::taskLoop() {
  do {
    diagnosticLogEvent("SMB serve-enter stop=%d", server.stop_requested);
    lastServeResult =
        smb2_serve_port(&server, kSmbListenBacklog, newClient, this);
    diagnosticLogEvent("SMB serve-exit result=%d stop=%d fd=%d",
                       lastServeResult, server.stop_requested, server.fd);
    // При закрытии TCP-контекста destructionHandler отвязывает его от уже
    // начатого APPEND. Перед остановкой или перезапуском listener дожидаемся,
    // пока core 1 вернёт кольцо, и только затем освобождаем handle/VFS.
    const uint32_t drainStarted = millis();
    while ((activeAsyncRead >= 0 || activeAsyncWrite >= 0 ||
            activeAsyncDirectory >= 0 || asyncDirectoryCount != 0 ||
            activeAsyncCreate >= 0 || asyncCreateCount != 0 ||
            activeAsyncClose >= 0 || asyncCloseCount != 0) &&
           static_cast<uint32_t>(millis() - drainStarted) <
               kMutateVfsTimeoutMs) {
      pollAsyncRead();
      pollAsyncWrite();
      pollAsyncDirectory();
      pollAsyncCreate();
      pollAsyncClose();
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (server.stop_requested == 0) {
      // Не оставляем плагину ложный вечный статус Listening. Слушатель
      // восстанавливается сам, а последняя операция показывает факт перезапуска.
      sendOperation("LISTENER RETRY");
      diagnosticLogEvent("SMB listener-retry delay=%lu",
                         static_cast<unsigned long>(kSmbListenerRetryMs));
      vTaskDelay(pdMS_TO_TICKS(kSmbListenerRetryMs));
    }
  } while (server.stop_requested == 0);
  for (size_t index = 0; index < kClientCount; ++index) {
    if (clients[index] != nullptr) {
      cleanupClient(clients[index]);
    }
  }
  releaseClientHandles(nullptr);
  runningFlag = false;
  taskAlive = false;
  task = nullptr;
  diagnosticLogEvent("SMB task-exit");
  vTaskDelete(nullptr);
}

void SmbServer::Impl::newClient(smb2_context* smb2, void* context) {
  auto* self = static_cast<Impl*>(context);
  if (self == nullptr || smb2 == nullptr) {
    diagnosticLogEvent("SMB client-invalid");
    return;
  }
  // Физический канал к Z80 односессионный, но соединений поверх него может
  // быть несколько: Проводник копирует одним, а эскизы и типы файлов тянет
  // другими. Вытеснять предыдущее соединение нельзя — вместе с ним умирали
  // его открытые файлы и шедшее копирование. Единственный канал к SD делят
  // те же activeSlot/closeActive, что делят открытые файлы одного сеанса.
  // Исправное соединение не закрывается никогда. Реестр нужен только для учёта
  // и для строки состояния в плагине; когда мест в нём нет, соединение всё
  // равно обслуживается, а его файлы освобождаются по владельцу дескриптора.
  // Отказ новичку клиент видит как обрыв на согласовании протокола: Dolphin
  // показывает «[102] сетевое соединение было разорвано», хотя сервер жив и
  // здоров. Он открывает три рабочих процесса сразу, и прежний предел в
  // четыре места выбирался одним файловым менеджером.
  const bool registered = self->addClient(smb2);
  diagnosticLogEvent("SMB client-accepted client=%p count=%u registered=%u",
                     smb2, static_cast<unsigned>(self->clientCount()),
                     registered ? 1U : 0U);
  // В серверной части libsmb2 6.1.0 расчёт pre-auth hash для SMB 3.1.1
  // пока несовместим с Windows: последний SESSION_SETUP получается с неверной
  // подписью, и Проводник показывает безликую ошибку 0x80004005. SMB 2.1,
  // 3.0 и 3.0.2 проверены теми же zx/zx и работают. Поэтому до отдельного
  // исправления 3.1.1 объявляем максимальным современный диалект SMB 3.0.2.
  smb2_set_version(smb2, SMB2_VERSION_0302);
  // До 0.6.85 этот вызов принудительно ставил SIGNING_REQUIRED. В исправленном 0.6.86
  // server.signing_enabled оставляет обязательный по протоколу ENABLED, а
  // ноль здесь не добавляет REQUIRED. Ожидаемый SecurityMode ответа — 0x0001.
  smb2_set_sign(smb2, 0);
  smb2_set_authentication(smb2, SMB2_SEC_NTLMSSP);
  int requestTimeoutSeconds = 120;
#ifdef ZIFI_HOST_BUILD
  // Нативная регрессия может сжать обычный тайм-аут PDU до одной секунды и
  // тем самым за несколько секунд проверить запросы, которые приложение
  // удерживает во внутренней очереди дольше сетевого тайм-аута.
  const char* const timeoutText = getenv("ZIFI_HOST_PDU_TIMEOUT_SECONDS");
  if (timeoutText != nullptr && timeoutText[0] != 0) {
    char* end = nullptr;
    const long parsed = strtol(timeoutText, &end, 10);
    if (end != timeoutText && *end == 0 && parsed > 0 && parsed <= 3600) {
      requestTimeoutSeconds = static_cast<int>(parsed);
    }
  }
#endif
  smb2_set_timeout(smb2, requestTimeoutSeconds);
  smb2_register_error_callback(smb2, libraryError);
  self->sendClientEvent(1);
}

void SmbServer::Impl::libraryError(smb2_context*, const char* message) {
  // UART занят двоичным протоколом, поэтому библиотечные сообщения нельзя
  // печатать в Serial. Во временной диагностической сборке ошибка попадает в
  // RAM-кольцо и не трогает ни flash, ни двоичный UART-протокол.
  diagnosticLogEvent("SMB library-error %s",
                     message == nullptr ? "(null)" : message);
}

bool SmbServer::Impl::addClient(smb2_context* smb2) {
  for (size_t index = 0; index < kClientCount; ++index) {
    if (clients[index] == smb2) {
      return true;
    }
  }
  for (size_t index = 0; index < kClientCount; ++index) {
    if (clients[index] == nullptr) {
      clients[index] = smb2;
      return true;
    }
  }
  return false;
}

void SmbServer::Impl::forgetClient(smb2_context* smb2) {
  for (size_t index = 0; index < kClientCount; ++index) {
    if (clients[index] == smb2) {
      clients[index] = nullptr;
    }
  }
}

bool SmbServer::Impl::knownClient(smb2_context* smb2) const {
  for (size_t index = 0; index < kClientCount; ++index) {
    if (smb2 != nullptr && clients[index] == smb2) {
      return true;
    }
  }
  return false;
}

size_t SmbServer::Impl::clientCount() const {
  size_t count = 0;
  for (size_t index = 0; index < kClientCount; ++index) {
    if (clients[index] != nullptr) {
      ++count;
    }
  }
  return count;
}

void SmbServer::Impl::resetHandles() {
  directoryCache.abortSnapshot();
  directoryCacheBuildSlot = -1;
  directoryCacheBuildGeneration = 0;
  memset(trees, 0, sizeof(trees));
  memset(handles, 0, sizeof(handles));
  memset(byteRangeLocks, 0, sizeof(byteRangeLocks));
  memset(pendingNotifies, 0, sizeof(pendingNotifies));
  activeSlot = -1;
  activeMode = ActiveMode::kNone;
  activeLogicalOffset = 0;
  activeVfsOffset = 0;
  activeRandomWrite = false;
}

void SmbServer::Impl::dropNotifiesForOwner(smb2_context* owner) {
  for (PendingNotify& pending : pendingNotifies) {
    if (pending.used && pending.context == owner) {
      pending = {};
    }
  }
}

void SmbServer::Impl::cancelNotifiesForHandle(smb2_context* owner, int slot,
                                               uint32_t generation,
                                               uint32_t status) {
  for (PendingNotify& pending : pendingNotifies) {
    if (!pending.used || pending.context != owner || pending.slot != slot ||
        pending.generation != generation) {
      continue;
    }
    smb2_context* const context = pending.context;
    const uint64_t messageId = pending.messageId;
    pending = {};
    if (!queueAsyncStatus(context, SMB2_CHANGE_NOTIFY, status, messageId)) {
      smb2_close_context(context);
    }
  }
}

bool SmbServer::Impl::cancelNotify(smb2_context* owner,
                                   uint64_t messageId) {
  for (PendingNotify& pending : pendingNotifies) {
    if (!pending.used || pending.context != owner ||
        pending.messageId != messageId) {
      continue;
    }
    pending = {};
    if (!queueAsyncStatus(owner, SMB2_CHANGE_NOTIFY,
                          SMB2_STATUS_CANCELLED, messageId)) {
      smb2_close_context(owner);
    }
    return true;
  }
  return false;
}

bool SmbServer::Impl::queueNotifyReply(PendingNotify& pending,
                                       uint32_t action,
                                       const char* relativeName,
                                       uint32_t secondAction,
                                       const char* secondRelativeName) {
  if (pending.context == nullptr || pending.messageId == 0 ||
      relativeName == nullptr || relativeName[0] == 0) {
    return false;
  }
  struct smb2_utf16* const utf16 = smb2_utf8_to_utf16(relativeName);
  if (utf16 == nullptr || utf16->len < 0) {
    free(utf16);
    return false;
  }
  struct smb2_utf16* secondUtf16 = nullptr;
  if (secondAction != 0 && secondRelativeName != nullptr &&
      secondRelativeName[0] != 0) {
    secondUtf16 = smb2_utf8_to_utf16(secondRelativeName);
    if (secondUtf16 == nullptr || secondUtf16->len < 0) {
      free(utf16);
      free(secondUtf16);
      return false;
    }
  }
  const size_t nameBytes = static_cast<size_t>(utf16->len) * 2;
  const size_t firstLength = 12 + nameBytes;
  const size_t firstStoredLength = secondUtf16 == nullptr
                                       ? firstLength
                                       : (firstLength + 3U) & ~size_t{3U};
  const size_t secondNameBytes = secondUtf16 == nullptr
                                     ? 0
                                     : static_cast<size_t>(secondUtf16->len) * 2;
  const size_t recordLength = firstStoredLength +
                              (secondUtf16 == nullptr ? 0
                                                     : 12 + secondNameBytes);
  if (recordLength > pending.outputBufferLength || recordLength > UINT32_MAX) {
    free(utf16);
    free(secondUtf16);
    return queueAsyncStatus(pending.context, SMB2_CHANGE_NOTIFY,
                            kStatusNotifyEnumDir, pending.messageId);
  }

  uint8_t* const record = static_cast<uint8_t*>(malloc(recordLength));
  if (record == nullptr) {
    free(utf16);
    free(secondUtf16);
    return false;
  }
  memset(record, 0, recordLength);
  writeLe32(record, secondUtf16 == nullptr
                        ? 0
                        : static_cast<uint32_t>(firstStoredLength));
  writeLe32(record + 4, action);
  writeLe32(record + 8, static_cast<uint32_t>(nameBytes));
  memcpy(record + 12, utf16->val, nameBytes);
  free(utf16);
  if (secondUtf16 != nullptr) {
    uint8_t* const second = record + firstStoredLength;
    writeLe32(second + 4, secondAction);
    writeLe32(second + 8, static_cast<uint32_t>(secondNameBytes));
    memcpy(second + 12, secondUtf16->val, secondNameBytes);
    free(secondUtf16);
  }

  smb2_change_notify_reply reply = {};
  reply.output_buffer_length = static_cast<uint32_t>(recordLength);
  reply.output = record;
  smb2_pdu* const pdu = smb2_cmd_change_notify_reply_async(
      pending.context, &reply, nullptr, nullptr);
  free(record);
  if (pdu == nullptr) {
    return false;
  }
  smb2_set_pdu_message_id(pending.context, pdu, pending.messageId);
  smb2_queue_pdu(pending.context, pdu);
  return true;
}

bool SmbServer::Impl::makeNotifyRelative(
    const PendingNotify& pending, const char* path,
    char relative[kMaxPath + 1]) const {
  if (path == nullptr || relative == nullptr) {
    return false;
  }
  char parent[kMaxPath + 1] = {};
  char name[kMaxPath + 1] = {};
  if (!splitParent(path, parent, name)) {
    return false;
  }
  if (!pending.watchTree) {
    if (!asciiEqualNoCase(parent, pending.path)) {
      return false;
    }
    snprintf(relative, kMaxPath + 1, "%s", name);
    return true;
  }
  if (!asciiPathBelongsTo(path, pending.path) ||
      asciiEqualNoCase(path, pending.path)) {
    return false;
  }
  const size_t prefix = strcmp(pending.path, "/") == 0
                            ? 1
                            : strlen(pending.path) + 1;
  if (prefix >= strlen(path) + 1) {
    return false;
  }
  snprintf(relative, kMaxPath + 1, "%s", path + prefix);
  for (char* cursor = relative; *cursor != 0; ++cursor) {
    if (*cursor == '/') {
      *cursor = '\\';
    }
  }
  return true;
}

void SmbServer::Impl::notifyChange(const char* path, uint32_t action,
                                   uint32_t filter) {
  if (path == nullptr || path[0] != '/') {
    return;
  }

  for (PendingNotify& pending : pendingNotifies) {
    if (!pending.used || (pending.completionFilter & filter) == 0) {
      continue;
    }

    const Handle* handle =
        pending.slot >= 0 &&
                static_cast<size_t>(pending.slot) < kHandleCount
            ? &handles[pending.slot]
            : nullptr;
    if (handle == nullptr || !handle->used ||
        handle->owner != pending.context ||
        handle->generation != pending.generation || !handle->directory) {
      smb2_context* const context = pending.context;
      const uint64_t messageId = pending.messageId;
      pending = {};
      if (!queueAsyncStatus(context, SMB2_CHANGE_NOTIFY,
                            SMB2_STATUS_FILE_CLOSED, messageId)) {
        smb2_close_context(context);
      }
      continue;
    }

    char relative[kMaxPath + 1] = {};
    if (!makeNotifyRelative(pending, path, relative)) {
      continue;
    }

    smb2_context* const context = pending.context;
    const bool queued = queueNotifyReply(pending, action, relative);
    pending = {};
    if (!queued) {
      smb2_close_context(context);
    }
  }
}

void SmbServer::Impl::notifyRename(const char* oldPath, const char* newPath,
                                   uint32_t filter) {
  if (oldPath == nullptr || newPath == nullptr) {
    return;
  }
  for (PendingNotify& pending : pendingNotifies) {
    if (!pending.used || (pending.completionFilter & filter) == 0) {
      continue;
    }
    const Handle* handle =
        pending.slot >= 0 && static_cast<size_t>(pending.slot) < kHandleCount
            ? &handles[pending.slot]
            : nullptr;
    if (handle == nullptr || !handle->used ||
        handle->owner != pending.context ||
        handle->generation != pending.generation || !handle->directory) {
      continue;
    }

    char oldRelative[kMaxPath + 1] = {};
    char newRelative[kMaxPath + 1] = {};
    const bool oldVisible =
        makeNotifyRelative(pending, oldPath, oldRelative);
    const bool newVisible =
        makeNotifyRelative(pending, newPath, newRelative);
    if (!oldVisible && !newVisible) {
      continue;
    }
    smb2_context* const context = pending.context;
    bool queued = false;
    if (oldVisible && newVisible) {
      queued = queueNotifyReply(
          pending, SMB2_NOTIFY_CHANGE_FILE_ACTION_RENAMED_OLD_NAME,
          oldRelative, SMB2_NOTIFY_CHANGE_FILE_ACTION_RENAMED_NEW_NAME,
          newRelative);
    } else {
      queued = queueNotifyReply(
          pending,
          oldVisible ? SMB2_NOTIFY_CHANGE_FILE_ACTION_RENAMED_OLD_NAME
                     : SMB2_NOTIFY_CHANGE_FILE_ACTION_RENAMED_NEW_NAME,
          oldVisible ? oldRelative : newRelative);
    }
    pending = {};
    if (!queued) {
      smb2_close_context(context);
    }
  }
}

void SmbServer::Impl::releaseByteRangeLocksForHandle(
    int slot, uint32_t generation) {
  if (slot < 0 || static_cast<size_t>(slot) >= kHandleCount) {
    return;
  }
  for (ByteRangeLock& lock : byteRangeLocks) {
    if (lock.used && lock.ownerSlot == static_cast<uint16_t>(slot) &&
        lock.ownerGeneration == generation) {
      lock = {};
    }
  }
}

bool SmbServer::Impl::lockRangeConflicts(
    uint64_t fileKey, int ownerSlot, uint32_t ownerGeneration,
    uint64_t offset, uint64_t length, bool exclusive) const {
  // По MS-FSA диапазон {0, 0} ни с чем не пересекается.
  if (length == 0) {
    return false;
  }
  const uint64_t last = offset + length - 1;
  for (const ByteRangeLock& lock : byteRangeLocks) {
    if (!lock.used || lock.fileKey != fileKey || lock.length == 0) {
      continue;
    }
    const uint64_t lockLast = lock.offset + lock.length - 1;
    if (offset > lockLast || last < lock.offset) {
      continue;
    }
    const bool sameOpen =
        lock.ownerSlot == static_cast<uint16_t>(ownerSlot) &&
        lock.ownerGeneration == ownerGeneration;
    if (lock.exclusive) {
      // Повторный пересекающийся exclusive lock запрещён даже тому же Open.
      if (!sameOpen || exclusive) {
        return true;
      }
    } else if (exclusive) {
      return true;
    }
  }
  return false;
}

bool SmbServer::Impl::fileIoConflictsWithLock(
    int slot, uint64_t offset, uint64_t length, bool writeAccess) const {
  if (slot < 0 || static_cast<size_t>(slot) >= kHandleCount ||
      !handles[slot].used || length == 0) {
    return false;
  }
  const Handle& handle = handles[slot];
  const uint64_t fileKey = directoryFileId(handle.path);
  const uint64_t last = offset + length - 1;
  for (const ByteRangeLock& lock : byteRangeLocks) {
    if (!lock.used || lock.fileKey != fileKey || lock.length == 0) {
      continue;
    }
    const uint64_t lockLast = lock.offset + lock.length - 1;
    if (offset > lockLast || last < lock.offset) {
      continue;
    }
    const bool sameOpen =
        lock.ownerSlot == static_cast<uint16_t>(slot) &&
        lock.ownerGeneration == handle.generation;
    if (lock.exclusive && !sameOpen) {
      return true;
    }
    // Shared lock разрешает чтение, но запрещает запись всем Open, включая
    // владельца этого shared lock — ровно как алгоритм MS-FSA 2.1.4.10.
    if (!lock.exclusive && writeAccess) {
      return true;
    }
  }
  return false;
}

bool SmbServer::Impl::allocateAsyncIoStorage() {
  releaseAsyncIoStorage();
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    asyncIoBuffers[index] = static_cast<uint8_t*>(heap_caps_malloc(
        kAsyncIoBaseSlotSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (asyncIoBuffers[index] == nullptr) {
      releaseAsyncIoStorage();
      return false;
    }
    asyncIoBufferCapacities[index] = kAsyncIoBaseSlotSize;
  }
  resetAsyncIo();
  return true;
}

void SmbServer::Impl::releaseAsyncIoStorage() {
  resetAsyncIo();
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    heap_caps_free(asyncIoBuffers[index]);
    asyncIoBuffers[index] = nullptr;
    asyncIoBufferCapacities[index] = 0;
  }
}

void SmbServer::Impl::resetAsyncIo() {
  clearQueuedReads();
  memset(asyncReads, 0, sizeof(asyncReads));
  memset(asyncWrites, 0, sizeof(asyncWrites));
  memset(asyncDirectories, 0, sizeof(asyncDirectories));
  memset(asyncCreates, 0, sizeof(asyncCreates));
  memset(asyncCloses, 0, sizeof(asyncCloses));
  activeAsyncRead = -1;
  activeAsyncWrite = -1;
  activeAsyncDirectory = -1;
  activeAsyncCreate = -1;
  activeAsyncClose = -1;
  asyncReadCount = 0;
  asyncWriteCount = 0;
  asyncDirectoryCount = 0;
  asyncCreateCount = 0;
  asyncCloseCount = 0;
  asyncVfsSequence = 0;
}

void SmbServer::Impl::clearQueuedReads() {
  while (queuedReadHead != nullptr) {
    QueuedRead* const current = queuedReadHead;
    queuedReadHead = current->next;
    heap_caps_free(current);
  }
  queuedReadTail = nullptr;
  queuedReadCount = 0;
}

uint64_t SmbServer::Impl::nextAsyncVfsSequence() {
  ++asyncVfsSequence;
  // Ноль означает «операция не поставлена в очередь». Теоретическое
  // переполнение не должно нарушать этот инвариант даже после очень долгой
  // работы без перезапуска.
  if (asyncVfsSequence == 0) {
    ++asyncVfsSequence;
  }
  return asyncVfsSequence;
}

uint64_t SmbServer::Impl::oldestAsyncReadSequence() const {
  uint64_t oldest = UINT64_MAX;
  for (const AsyncRead& pending : asyncReads) {
    if (pending.used && !pending.cancelRequested && pending.sequence != 0 &&
        pending.sequence < oldest) {
      oldest = pending.sequence;
    }
  }
  for (const QueuedRead* pending = queuedReadHead; pending != nullptr;
       pending = pending->next) {
    if (pending->sequence != 0 && pending->sequence < oldest) {
      oldest = pending->sequence;
    }
  }
  return oldest;
}

uint64_t SmbServer::Impl::oldestAsyncWriteSequence() const {
  uint64_t oldest = UINT64_MAX;
  for (const AsyncWrite& pending : asyncWrites) {
    if (pending.used && !pending.cancelRequested && pending.sequence != 0 &&
        pending.sequence < oldest) {
      oldest = pending.sequence;
    }
  }
  return oldest;
}

uint64_t SmbServer::Impl::oldestAsyncDirectorySequence() const {
  uint64_t oldest = UINT64_MAX;
  for (const AsyncDirectory& pending : asyncDirectories) {
    if (pending.used && !pending.cancelRequested && pending.sequence != 0 &&
        pending.sequence < oldest) {
      oldest = pending.sequence;
    }
  }
  return oldest;
}

uint64_t SmbServer::Impl::oldestAsyncCreateSequence() const {
  uint64_t oldest = UINT64_MAX;
  for (const AsyncCreate& pending : asyncCreates) {
    if (pending.used && !pending.cancelRequested && pending.sequence != 0 &&
        pending.sequence < oldest) {
      oldest = pending.sequence;
    }
  }
  return oldest;
}

uint64_t SmbServer::Impl::oldestAsyncCloseSequence() const {
  uint64_t oldest = UINT64_MAX;
  for (const AsyncClose& pending : asyncCloses) {
    if (pending.used && !pending.cancelRequested && pending.sequence != 0 &&
        pending.sequence < oldest) {
      oldest = pending.sequence;
    }
  }
  return oldest;
}

int SmbServer::Impl::allocateAsyncRead() {
  if (asyncReadCount >= kAsyncReadServiceDepth) {
    return -1;
  }
  if (asyncReadCount + asyncWriteCount >= kAsyncIoQueueDepth) {
    return -1;
  }
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    if (!asyncReads[index].used && !asyncWrites[index].used &&
        asyncIoBuffers[index] != nullptr) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

int SmbServer::Impl::allocateAsyncWrite(size_t requiredCapacity) {
  if (asyncReadCount + asyncWriteCount >= kAsyncIoQueueDepth) {
    return -1;
  }
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    if (!asyncReads[index].used && !asyncWrites[index].used &&
        asyncIoBuffers[index] != nullptr) {
      if (asyncIoBufferCapacities[index] < requiredCapacity) {
        // Полный кэш ранее прочитанного файла полезен только для повторного
        // READ. Перед большим WRITE он не должен отнять место одновременно у
        // входного PDU libsmb2 и независимого буфера физической записи.
        dropFileCache();
        uint8_t* grown = static_cast<uint8_t*>(heap_caps_malloc(
            requiredCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (grown == nullptr) {
          continue;
        }
        heap_caps_free(asyncIoBuffers[index]);
        asyncIoBuffers[index] = grown;
        asyncIoBufferCapacities[index] = requiredCapacity;
      }
      return static_cast<int>(index);
    }
  }
  return -1;
}

bool SmbServer::Impl::enqueueAsyncRead(smb2_context* context,
                                       uint64_t messageId, int slot,
                                       uint64_t sequence,
                                       uint32_t generation, uint32_t offset,
                                       uint32_t length) {
  if (context == nullptr || messageId == 0 || slot < 0 || length == 0) {
    return false;
  }
  // STATUS_PENDING временно возвращает жизнь запросу Windows. Даже если
  // клиент ведёт себя агрессивно, описатели не должны расти без границы.
  if (queuedReadCount + asyncReadCount + asyncWriteCount >=
      kAsyncIoQueueDepth) {
    return false;
  }
  QueuedRead* pending = static_cast<QueuedRead*>(heap_caps_calloc(
      1, sizeof(QueuedRead), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (pending == nullptr) {
    pending = static_cast<QueuedRead*>(heap_caps_calloc(
        1, sizeof(QueuedRead), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (pending == nullptr) {
    return false;
  }
  pending->context = context;
  pending->messageId = messageId;
  pending->sequence = sequence;
  pending->slot = slot;
  pending->generation = generation;
  pending->offset = offset;
  pending->length = length;
  pending->requestStartedMs = millis();
  if (queuedReadTail == nullptr) {
    queuedReadHead = pending;
  } else {
    queuedReadTail->next = pending;
  }
  queuedReadTail = pending;
  ++queuedReadCount;
  diagnosticLogEvent("SMB read-deferred off=%lu len=%lu waiting=%u",
                     static_cast<unsigned long>(offset),
                     static_cast<unsigned long>(length),
                     static_cast<unsigned>(queuedReadCount));
  return true;
}

bool SmbServer::Impl::beginAsyncRead(smb2_context* context,
                                     uint64_t messageId, int slot,
                                     uint64_t sequence, uint32_t generation,
                                     uint32_t offset, uint32_t length,
                                     uint32_t requestStartedMs,
                                     bool pendingSent, int index) {
  if (context == nullptr || messageId == 0 || index < 0 ||
      static_cast<size_t>(index) >= kAsyncIoQueueDepth ||
      asyncIoBuffers[index] == nullptr || asyncReads[index].used ||
      asyncWrites[index].used) {
    return false;
  }

  AsyncRead& pending = asyncReads[index];
  pending = {};
  pending.used = true;
  pending.context = context;
  pending.owner = context;
  pending.messageId = messageId;
  pending.sequence = sequence;
  pending.slot = slot;
  pending.generation = generation;
  pending.offset = offset;
  pending.length = length;
  pending.requestStartedMs = requestStartedMs;
  pending.pendingSent = pendingSent;
  pending.lastProgressMs = millis();
  ++asyncReadCount;
  return true;
}

void SmbServer::Impl::promoteQueuedReads() {
  while (queuedReadHead != nullptr) {
    const int index = allocateAsyncRead();
    if (index < 0) {
      return;
    }

    QueuedRead* const queued = queuedReadHead;
    queuedReadHead = queued->next;
    if (queuedReadHead == nullptr) {
      queuedReadTail = nullptr;
    }
    if (queuedReadCount != 0) {
      --queuedReadCount;
    }

    const bool validHandle =
        queued->slot >= 0 &&
        static_cast<size_t>(queued->slot) < kHandleCount &&
        handles[queued->slot].used &&
        handles[queued->slot].owner == queued->context &&
        handles[queued->slot].generation == queued->generation;
    if (!validHandle) {
      const bool replyQueued = queueAsyncStatus(
          queued->context, SMB2_READ, SMB2_STATUS_FILE_CLOSED,
          queued->messageId);
      smb2_context* const context = queued->context;
      heap_caps_free(queued);
      restoreServerCreditsIfIoIdle(context);
      if (!replyQueued) {
        smb2_close_context(context);
      }
      continue;
    }

    const bool started = beginAsyncRead(
        queued->context, queued->messageId, queued->slot,
        queued->sequence, queued->generation, queued->offset,
        queued->length, queued->requestStartedMs, queued->pendingSent, index);
    smb2_context* const context = queued->context;
    const uint32_t offset = queued->offset;
    heap_caps_free(queued);
    if (!started) {
      smb2_close_context(context);
      return;
    }
    diagnosticLogEvent("SMB read-promoted off=%lu waiting=%u",
                       static_cast<unsigned long>(offset),
                       static_cast<unsigned>(queuedReadCount));
  }
}

void SmbServer::Impl::failQueuedReads(uint32_t status) {
  while (queuedReadHead != nullptr) {
    QueuedRead* const queued = queuedReadHead;
    queuedReadHead = queued->next;
    smb2_context* const context = queued->context;
    const uint64_t messageId = queued->messageId;
    heap_caps_free(queued);
    if (queuedReadCount != 0) {
      --queuedReadCount;
    }
    restoreServerCreditsIfIoIdle(context);
    const bool replyQueued =
        queueAsyncStatus(context, SMB2_READ, status, messageId);
    if (!replyQueued) {
      smb2_close_context(context);
    }
  }
  queuedReadTail = nullptr;
  queuedReadCount = 0;
}

void SmbServer::Impl::dropQueuedReadsForOwner(smb2_context* owner) {
  QueuedRead* previous = nullptr;
  QueuedRead* current = queuedReadHead;
  while (current != nullptr) {
    QueuedRead* const next = current->next;
    if (current->context != owner) {
      previous = current;
      current = next;
      continue;
    }
    if (previous == nullptr) {
      queuedReadHead = next;
    } else {
      previous->next = next;
    }
    if (queuedReadTail == current) {
      queuedReadTail = previous;
    }
    heap_caps_free(current);
    if (queuedReadCount != 0) {
      --queuedReadCount;
    }
    current = next;
  }
}

bool SmbServer::Impl::cancelQueuedRead(smb2_context* owner,
                                       uint64_t messageId) {
  QueuedRead* previous = nullptr;
  QueuedRead* current = queuedReadHead;
  while (current != nullptr) {
    if (current->context != owner || current->messageId != messageId) {
      previous = current;
      current = current->next;
      continue;
    }
    if (previous == nullptr) {
      queuedReadHead = current->next;
    } else {
      previous->next = current->next;
    }
    if (queuedReadTail == current) {
      queuedReadTail = previous;
    }
    const uint64_t targetMessageId = current->messageId;
    heap_caps_free(current);
    if (queuedReadCount != 0) {
      --queuedReadCount;
    }
    restoreServerCreditsIfIoIdle(owner);
    const bool queued = queueAsyncStatus(
        owner, SMB2_READ, SMB2_STATUS_CANCELLED, targetMessageId);
    if (!queued) {
      smb2_close_context(owner);
    }
    return true;
  }
  return false;
}

int SmbServer::Impl::findReadyAsyncRead() const {
  if (activeAsyncRead >= 0 || activeAsyncWrite >= 0 || activeAsyncClose >= 0) {
    return -1;
  }

  int oldestIndex = -1;
  uint64_t oldestSequence = UINT64_MAX;
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    const AsyncRead& pending = asyncReads[index];
    if (!pending.used || pending.inFlight || pending.cancelRequested ||
        pending.slot < 0 ||
        static_cast<size_t>(pending.slot) >= kHandleCount) {
      continue;
    }
    const Handle& handle = handles[pending.slot];
    if (!handle.used || pending.generation != handle.generation) {
      continue;
    }
    if (pending.sequence < oldestSequence) {
      oldestSequence = pending.sequence;
      oldestIndex = static_cast<int>(index);
    }
  }
  return oldestIndex;
}

int SmbServer::Impl::findReadyAsyncWrite() const {
  if (activeAsyncWrite >= 0 || activeAsyncRead >= 0 || activeAsyncClose >= 0) {
    return -1;
  }

  int oldestIndex = -1;
  uint64_t oldestSequence = UINT64_MAX;
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    const AsyncWrite& pending = asyncWrites[index];
    if (!pending.used || pending.inFlight || pending.cancelRequested ||
        pending.slot < 0 ||
        static_cast<size_t>(pending.slot) >= kHandleCount) {
      continue;
    }
    const Handle& handle = handles[pending.slot];
    if (!handle.used || pending.generation != handle.generation) {
      continue;
    }
    if (pending.sequence < oldestSequence) {
      oldestSequence = pending.sequence;
      oldestIndex = static_cast<int>(index);
    }
  }
  return oldestIndex;
}

bool SmbServer::Impl::hasAsyncWritesForHandle(int slot,
                                              uint32_t generation) const {
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    const AsyncWrite& pending = asyncWrites[index];
    if (pending.used && pending.slot == slot &&
        pending.generation == generation) {
      return true;
    }
  }
  return false;
}

bool SmbServer::Impl::hasAsyncIoForOwner(smb2_context* owner) const {
  if (owner == nullptr) {
    return false;
  }
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    if ((asyncReads[index].used && asyncReads[index].owner == owner) ||
        (asyncWrites[index].used && asyncWrites[index].owner == owner)) {
      return true;
    }
  }
  for (const QueuedRead* pending = queuedReadHead; pending != nullptr;
       pending = pending->next) {
    if (pending->context == owner) {
      return true;
    }
  }
  for (const AsyncCreate& pending : asyncCreates) {
    if (pending.used && pending.owner == owner) {
      return true;
    }
  }
  for (const AsyncClose& pending : asyncCloses) {
    if (pending.used && pending.owner == owner) {
      return true;
    }
  }
  return false;
}

void SmbServer::Impl::releaseDetachedOwnerIfIdle(smb2_context* owner) {
  if (owner == nullptr || hasAsyncIoForOwner(owner)) {
    return;
  }
  // owner используется только как идентификатор; разрушенный smb2_context
  // здесь не разыменовывается.
  releaseClientHandles(owner);
  diagnosticLogEvent("SMB cleanup-deferred done");
}

bool SmbServer::Impl::drainAsyncWritesForHandle(int slot,
                                                uint32_t generation) {
  for (;;) {
    if (!hasAsyncWritesForHandle(slot, generation)) {
      return slot >= 0 && static_cast<size_t>(slot) < kHandleCount &&
             handles[slot].used && handles[slot].generation == generation &&
             !handles[slot].failed;
    }
    pollAsyncWrite();
    // pollAsyncWrite() мог только что завершить последний WRITE этого handle.
    // Не трактуем пустую после опроса очередь как отсутствие прогресса.
    if (!hasAsyncWritesForHandle(slot, generation)) {
      return slot >= 0 && static_cast<size_t>(slot) < kHandleCount &&
             handles[slot].used && handles[slot].generation == generation &&
             !handles[slot].failed;
    }
    const uint32_t now = millis();
    bool globalProgressIsRecent = false;
    for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
      if (asyncWrites[index].used &&
          static_cast<uint32_t>(now - asyncWrites[index].lastProgressMs) <
              kAsyncIoProgressTimeoutMs) {
        globalProgressIsRecent = true;
        break;
      }
    }
    // Таймаут измеряет отсутствие прогресса, а не полную длительность очереди.
    // Иначе корректная медленная запись нескольких блоков обрывалась ровно на
    // 90-й секунде, даже когда каждое окно продолжало подтверждаться Z80.
    if (!globalProgressIsRecent) {
      failAsyncWrites(SMB2_STATUS_IO_TIMEOUT);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

bool SmbServer::Impl::asyncIoTimedOut() const {
  const uint32_t now = millis();
  // Ожидающий слот не завис: он закономерно стоит за активным обменом. Следим
  // только за владельцем единственного физического VFS-окна.
  if (activeAsyncRead >= 0 &&
      static_cast<size_t>(activeAsyncRead) < kAsyncIoQueueDepth &&
      asyncReads[activeAsyncRead].used &&
      static_cast<uint32_t>(now - asyncReads[activeAsyncRead].lastProgressMs) >=
          kAsyncIoProgressTimeoutMs) {
    return true;
  }
  if (activeAsyncWrite >= 0 &&
      static_cast<size_t>(activeAsyncWrite) < kAsyncIoQueueDepth &&
      asyncWrites[activeAsyncWrite].used &&
      static_cast<uint32_t>(now - asyncWrites[activeAsyncWrite].lastProgressMs) >=
          kAsyncIoProgressTimeoutMs) {
    return true;
  }
  return false;
}

void SmbServer::Impl::discardAsyncReadData(int index) {
  if (index < 0 || static_cast<size_t>(index) >= kAsyncIoQueueDepth ||
      asyncIoBuffers[index] == nullptr) {
    return;
  }
  while (bridge.vfsToNetworkAvailable() != 0) {
    const size_t part = minimum(bridge.vfsToNetworkAvailable(),
                                asyncIoBufferCapacities[index]);
    if (bridge.readForNetwork(asyncIoBuffers[index], part) == 0) {
      break;
    }
  }
}

void SmbServer::Impl::completeAsyncReadWithStatus(int index, uint32_t status,
                                                  bool closePhysical) {
  if (index < 0 || static_cast<size_t>(index) >= kAsyncIoQueueDepth ||
      !asyncReads[index].used) {
    return;
  }

  AsyncRead& pending = asyncReads[index];
  smb2_context* const context = pending.context;
  smb2_context* const owner = pending.owner;
  const uint64_t messageId = pending.messageId;
  const bool detached = context == nullptr;
  if (activeAsyncRead == index) {
    activeAsyncRead = -1;
  }
  pending = {};
  if (asyncReadCount != 0) {
    --asyncReadCount;
  }

  if (closePhysical) {
    closeActive(false);
  }
  restoreServerCreditsIfIoIdle(context);
  const bool replyQueued =
      context == nullptr || queueAsyncStatus(context, SMB2_READ, status,
                                             messageId);
  if (detached) {
    releaseDetachedOwnerIfIdle(owner);
  }
  if (!replyQueued) {
    smb2_close_context(context);
  }
}

void SmbServer::Impl::completeCancelledAsyncWrite(int index,
                                                  bool closePhysical) {
  if (index < 0 || static_cast<size_t>(index) >= kAsyncIoQueueDepth ||
      !asyncWrites[index].used) {
    return;
  }
  AsyncWrite& pending = asyncWrites[index];
  smb2_context* const context = pending.context;
  smb2_context* const owner = pending.owner;
  const uint64_t messageId = pending.messageId;
  const bool needsReply = context != nullptr && !pending.replied;
  const bool detached = context == nullptr;
  if (activeAsyncWrite == index) {
    activeAsyncWrite = -1;
  }
  pending = {};
  if (asyncWriteCount != 0) {
    --asyncWriteCount;
  }
  if (closePhysical) {
    // Уже подтверждённое Z80 окно не откатываем: отменяется остаток одной
    // SMB-команды, а физический поток закрывается в согласованном состоянии.
    closeActive(true);
  }
  restoreServerCreditsIfIoIdle(context);
  if (needsReply &&
      !queueAsyncStatus(context, SMB2_WRITE, SMB2_STATUS_CANCELLED,
                        messageId)) {
    smb2_close_context(context);
  }
  if (detached) {
    releaseDetachedOwnerIfIdle(owner);
  }
}

void SmbServer::Impl::failAsyncReads(uint32_t status) {
  // Отменяемое чтение могло оставить на мосту незавершённое окно. Пока обмен
  // не отобран, мост занят, и следом не проходит даже аварийное закрытие
  // файла — сервер остаётся жив, но к карте больше не обращается.
  if (bridge.requestPending() && !bridge.reclaim(kNormalVfsTimeoutMs)) {
    // Core 1 всё ещё пользуется общим Exchange. Не освобождаем слот и не
    // отправляем поверх него следующий запрос: pollAsyncRead заберёт поздний
    // ответ, отбросит данные отменённого READ и только тогда продолжит очередь.
    const uint32_t now = millis();
    for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
      if (asyncReads[index].used) {
        asyncReads[index].cancelRequested = true;
        asyncReads[index].lastProgressMs = now;
      }
    }
    diagnosticLogEvent("SMB bridge-reclaim-timeout op=%u",
                       static_cast<unsigned>(bridge.pendingOperation()));
    failQueuedReads(status);
    return;
  }
  discardAsyncReadData(activeAsyncRead);
  smb2_context* contextToClose = nullptr;
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    AsyncRead& pending = asyncReads[index];
    if (!pending.used) {
      continue;
    }
    smb2_context* const context = pending.context;
    const uint64_t messageId = pending.messageId;
    pending = {};
    restoreServerCreditsIfIoIdle(context);
    if (context != nullptr &&
        !queueAsyncStatus(context, SMB2_READ, status, messageId)) {
      contextToClose = context;
    }
  }
  activeAsyncRead = -1;
  asyncReadCount = 0;
  failQueuedReads(status);
  closeActive(false);
  if (contextToClose != nullptr) {
    smb2_close_context(contextToClose);
  }
}

void SmbServer::Impl::failAsyncWrites(uint32_t status) {
  // То же и для записи: брошенное окно держит мост навсегда.
  if (bridge.requestPending() && !bridge.reclaim(kMutateVfsTimeoutMs)) {
    const uint32_t now = millis();
    for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
      if (asyncWrites[index].used) {
        asyncWrites[index].cancelRequested = true;
        asyncWrites[index].lastProgressMs = now;
      }
    }
    diagnosticLogEvent("SMB bridge-reclaim-timeout op=%u",
                       static_cast<unsigned>(bridge.pendingOperation()));
    return;
  }
  smb2_context* contextToClose = nullptr;
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    AsyncWrite& pending = asyncWrites[index];
    if (!pending.used) {
      continue;
    }
    if (pending.slot >= 0 &&
        static_cast<size_t>(pending.slot) < kHandleCount &&
        handles[pending.slot].used &&
        handles[pending.slot].generation == pending.generation) {
      handles[pending.slot].failed = true;
    }
    smb2_context* const context = pending.context;
    const uint64_t messageId = pending.messageId;
    const bool needsReply = context != nullptr && !pending.replied;
    pending = {};
    restoreServerCreditsIfIoIdle(context);
    if (needsReply &&
        !queueAsyncStatus(context, SMB2_WRITE, status, messageId)) {
      contextToClose = context;
    }
  }
  activeAsyncWrite = -1;
  asyncWriteCount = 0;
  closeActive(false);
  if (contextToClose != nullptr) {
    smb2_close_context(contextToClose);
  }
}

bool SmbServer::Impl::allocateDirectoryBatch() {
  releaseDirectoryBatch();
  if (!directoryCache.enabled()) {
    return false;
  }
  const size_t stride = padTo8(sizeof(smb2_fileidbothdirectoryinformation));
  directoryBatch = static_cast<uint8_t*>(heap_caps_calloc(
      kDirectoryBatchCapacity, stride,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  directoryBatchNames = static_cast<char*>(heap_caps_calloc(
      kDirectoryBatchCapacity, kMaxPath + 1,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (directoryBatch == nullptr || directoryBatchNames == nullptr) {
    heap_caps_free(directoryBatch);
    heap_caps_free(directoryBatchNames);
    directoryBatch = nullptr;
    directoryBatchNames = nullptr;
    return false;
  }
  return true;
}

void SmbServer::Impl::releaseDirectoryBatch() {
  heap_caps_free(directoryBatch);
  heap_caps_free(directoryBatchNames);
  directoryBatch = nullptr;
  directoryBatchNames = nullptr;
  // Копия файла живёт в той же PSRAM и вне работающего сервера смысла не имеет.
  dropFileCache();
}


uint32_t SmbServer::Impl::allocateTree(bool ipc, smb2_context* owner) {
  size_t slot = kTreeCount;
  for (size_t index = 0; index < kTreeCount; ++index) {
    if (!trees[index].used) {
      slot = index;
      break;
    }
  }
  if (slot == kTreeCount) {
    return 0;
  }

  uint32_t id = 0;
  do {
    id = nextTreeId++;
    if (nextTreeId == 0) {
      nextTreeId = kFirstTreeId;
    }
  } while (id == 0 || id == 0xDEADBEEFUL || findTree(id) != nullptr);

  trees[slot].used = true;
  trees[slot].ipc = ipc;
  trees[slot].id = id;
  trees[slot].owner = owner;
  return id;
}

const SmbServer::Impl::Tree* SmbServer::Impl::findTree(uint32_t id) const {
  for (size_t index = 0; index < kTreeCount; ++index) {
    if (trees[index].used && trees[index].id == id) {
      return &trees[index];
    }
  }
  return nullptr;
}

void SmbServer::Impl::releaseTree(uint32_t id) {
  for (size_t index = 0; index < kTreeCount; ++index) {
    if (trees[index].used && trees[index].id == id) {
      memset(&trees[index], 0, sizeof(trees[index]));
      return;
    }
  }
}

void SmbServer::Impl::cleanupClient(smb2_context* smb2) {
  if (smb2 == nullptr) {
    return;
  }
  diagnosticLogEvent("SMB cleanup-owner client=%p count=%u known=%u", smb2,
                     static_cast<unsigned>(clientCount()),
                     knownClient(smb2) ? 1U : 0U);
  // Эти запросы ещё синхронны и не владеют PSRAM/VFS. При разрушении transport
  // их исходные PDU всё равно освобождает libsmb2.
  dropQueuedReadsForOwner(smb2);
  dropAsyncDirectoriesForOwner(smb2);
  dropAsyncCreatesForOwner(smb2);
  dropAsyncClosesForOwner(smb2);
  dropNotifiesForOwner(smb2);
  bool hadAsyncIo = hasAsyncIoForOwner(smb2);
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    AsyncRead& pending = asyncReads[index];
    if (!pending.used || pending.context != smb2) {
      continue;
    }
    if (activeAsyncRead == static_cast<int>(index) || pending.inFlight) {
      hadAsyncIo = true;
      pending.context = nullptr;
      pending.cancelRequested = true;
      continue;
    }
    // Этот READ ещё не владеет ни мостом, ни физическим файлом. Удаляем ровно
    // его слот сразу. Оставлять отменённый запрос рядом с активным READ другого
    // клиента до конца 64-КиБ ответа на железе приводило к IO_DEVICE_ERROR у
    // выжившего соединения; общий resetAsyncIo здесь по-прежнему недопустим.
    pending = {};
    if (asyncReadCount != 0) {
      --asyncReadCount;
    }
    diagnosticLogEvent("SMB cleanup-drop queued-read index=%u",
                       static_cast<unsigned>(index));
  }
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    AsyncWrite& pending = asyncWrites[index];
    if (!pending.used || pending.context != smb2) {
      continue;
    }
    hadAsyncIo = true;
    // Все уже принятые payload находятся в независимых PSRAM-слотах. После
    // разрыва соединения дописываем их на SD до согласованной границы окна,
    // хотя отправить финальный SMB-ответ уже невозможно.
    pending.context = nullptr;
    pending.replied = true;
    pending.cancelRequested = false;
  }
  if (hadAsyncIo) {
    // Core 1 ещё владеет UART и одним из колец. Окончательную очистку выполнит
    // serviceHandler после возврата текущего VFS-окна. Даже ещё не запущенный
    // READ нельзя убирать общим resetAsyncIo(): рядом может выполняться запрос
    // другого клиента.
    forgetClient(smb2);
    sendClientEvent(clientCount() == 0 ? 0 : 2);
    diagnosticLogEvent("SMB cleanup-deferred async-io");
    return;
  }
  releaseClientHandles(smb2);
  forgetClient(smb2);
  // Следующий клиент начинает с чистого поля индикации, а дедупликация не
  // должна проглотить его первое событие из-за совпадения с прошлым сеансом.
  if (clientCount() == 0) {
    lastOperationText[0] = 0;
    resetProgress();
  }
  sendClientEvent(clientCount() == 0 ? 0 : 2);
  diagnosticLogEvent("SMB cleanup-done count=%u",
                     static_cast<unsigned>(clientCount()));
}

// Освобождает имущество одного соединения. owner == nullptr означает уборку
// осиротевших дескрипторов: их владелец уже ушёл, а отложенный обмен на core 1
// закончился только сейчас.
void SmbServer::Impl::releaseClientHandles(smb2_context* owner) {
  // Физический файл на Z80 закрываем только если он принадлежал уходящему
  // соединению: чужое чтение посреди окна прерывать нельзя.
  if (activeSlot >= 0 && static_cast<size_t>(activeSlot) < kHandleCount) {
    const Handle& active = handles[activeSlot];
    const bool mine = owner != nullptr ? active.owner == owner
                                       : !knownClient(active.owner);
    if (active.used && mine) {
      closeActive(true);
    }
  }
  for (size_t index = 0; index < kHandleCount; ++index) {
    Handle& handle = handles[index];
    if (!handle.used) {
      continue;
    }
    const bool mine = owner != nullptr ? handle.owner == owner
                                       : !knownClient(handle.owner);
    if (!mine) {
      continue;
    }
    cancelNotifiesForHandle(handle.owner, static_cast<int>(index),
                            handle.generation, SMB2_STATUS_CANCELLED);
    if (handle.deletePending && strcmp(handle.path, "/") != 0) {
      removePath(handle.path);
    } else if (handle.metadataDirty) {
      invalidateParent(handle.path);
    }
    releaseByteRangeLocksForHandle(static_cast<int>(index),
                                   handle.generation);
    clearTransferProgress(handle, true);
    memset(&handle, 0, sizeof(handle));
    if (activeSlot == static_cast<int>(index)) {
      activeSlot = -1;
      activeMode = ActiveMode::kNone;
      activeLogicalOffset = 0;
      activeVfsOffset = 0;
      activeRandomWrite = false;
    }
  }
  for (size_t index = 0; index < kTreeCount; ++index) {
    Tree& tree = trees[index];
    if (!tree.used) {
      continue;
    }
    const bool mine = owner != nullptr ? tree.owner == owner
                                       : !knownClient(tree.owner);
    if (mine) {
      memset(&tree, 0, sizeof(tree));
    }
  }
  if (activeAsyncRead < 0 && activeAsyncWrite < 0 &&
      activeAsyncDirectory < 0 && activeAsyncCreate < 0 &&
      activeAsyncClose < 0 &&
      clientCount() == 0) {
    resetAsyncIo();
  }
}

bool SmbServer::Impl::releaseTreeHandles(smb2_context* owner,
                                         uint32_t treeId) {
  if (owner == nullptr || treeId == 0) {
    return false;
  }

  // Запросы READ, ещё не получившие STATUS_PENDING, тоже ссылаются на открытые
  // объекты.
  // Отвечаем им CANCELLED до ответа TREE_DISCONNECT и удаляем только запросы
  // закрываемого дерева; очереди соседних TreeConnect/клиентов не трогаем.
  QueuedRead* previous = nullptr;
  QueuedRead* current = queuedReadHead;
  while (current != nullptr) {
    QueuedRead* const next = current->next;
    const bool matches =
        current->slot >= 0 &&
        static_cast<size_t>(current->slot) < kHandleCount &&
        handles[current->slot].used &&
        handles[current->slot].owner == owner &&
        handles[current->slot].treeId == treeId &&
        handles[current->slot].generation == current->generation;
    if (!matches) {
      previous = current;
      current = next;
      continue;
    }
    if (previous == nullptr) {
      queuedReadHead = next;
    } else {
      previous->next = next;
    }
    if (queuedReadTail == current) {
      queuedReadTail = previous;
    }
    if (queuedReadCount != 0) {
      --queuedReadCount;
    }
    const bool replyQueued = queueAsyncStatus(
        current->context, SMB2_READ, SMB2_STATUS_CANCELLED,
        current->messageId);
    smb2_context* const context = current->context;
    heap_caps_free(current);
    restoreServerCreditsIfIoIdle(context);
    if (!replyQueued) {
      return false;
    }
    current = next;
  }

  // Уже асинхронные READ завершаем до удаления открытого объекта. Активное
  // физическое окно сначала возвращается с ядра 1, поэтому его буфер нельзя
  // освобождать сразу.
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    AsyncRead& pending = asyncReads[index];
    if (!pending.used || pending.slot < 0 ||
        static_cast<size_t>(pending.slot) >= kHandleCount) {
      continue;
    }
    const Handle& handle = handles[pending.slot];
    if (handle.used && handle.owner == owner && handle.treeId == treeId &&
        handle.generation == pending.generation) {
      pending.cancelRequested = true;
    }
  }
  const uint32_t cancelStarted = millis();
  for (;;) {
    bool pendingTreeRead = false;
    for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
      const AsyncRead& pending = asyncReads[index];
      if (!pending.used || pending.slot < 0 ||
          static_cast<size_t>(pending.slot) >= kHandleCount) {
        continue;
      }
      const Handle& handle = handles[pending.slot];
      if (handle.used && handle.owner == owner && handle.treeId == treeId &&
          handle.generation == pending.generation) {
        pendingTreeRead = true;
        break;
      }
    }
    if (!pendingTreeRead) {
      break;
    }
    pollAsyncRead();
    if (static_cast<uint32_t>(millis() - cancelStarted) >=
        kAsyncIoProgressTimeoutMs) {
      diagnosticLogEvent("SMB untree-read-timeout tree=%08lx",
                         static_cast<unsigned long>(treeId));
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  // Перед закрытием дерева завершаем принятые WRITE и применяем ту же
  // финализацию открытого объекта, что на CLOSE.
  for (size_t index = 0; index < kHandleCount; ++index) {
    Handle& handle = handles[index];
    if (!handle.used || handle.owner != owner || handle.treeId != treeId) {
      continue;
    }
    const int slot = static_cast<int>(index);
    if (!drainAsyncWritesForHandle(slot, handle.generation)) {
      handle.failed = true;
    }
    if (!handle.failed && handle.sizeReserved &&
        handle.ownsSizeReservation &&
        !commitReservedSize(slot)) {
      handle.sizeReserved = false;
      handle.ownsSizeReservation = false;
      handle.failed = true;
    }
    if (!handle.failed && handle.metadataPending &&
        !applyPendingMetadata(slot)) {
      handle.metadataPending = false;
      handle.failed = true;
    }
    if (activeSlot == slot) {
      if (handle.writable) {
        if (!closeActive(!handle.failed)) {
          handle.failed = true;
        }
      } else {
        closeActive(true);
      }
    }
    const bool shouldDelete =
        handle.deletePending || (handle.failed && handle.createdNew);
    if (shouldDelete && strcmp(handle.path, "/") != 0) {
      removePath(handle.path);
    } else if (handle.metadataDirty) {
      invalidateParent(handle.path);
    }
    cancelNotifiesForHandle(handle.owner, slot, handle.generation,
                            SMB2_STATUS_CANCELLED);
    releaseByteRangeLocksForHandle(slot, handle.generation);
    clearTransferProgress(handle, true);
    memset(&handle, 0, sizeof(handle));
  }
  return true;
}

void SmbServer::Impl::sendClientEvent(uint8_t state) {
  if (eventSink != nullptr) {
    eventSink(eventContext, kEventSmbClient, &state, 1);
  }
}

void SmbServer::Impl::sendOperation(const char* operation, const char* path) {
  if (eventSink == nullptr || operation == nullptr) {
    return;
  }
  char shown[32] = {};
  size_t used = 0;
  while (*operation != 0 && used + 1 < sizeof(shown)) {
    shown[used++] = *operation++;
  }
  if (path != nullptr && *path != 0 && used + 1 < sizeof(shown)) {
    shown[used++] = ' ';
    while (*path != 0 && used + 1 < sizeof(shown)) {
      shown[used++] = *path++;
    }
  }
  shown[used] = 0;
  // Копирование одного файла порождает тысячи одинаковых строк «WRITE /путь».
  // Каждая из них уходила отдельным пакетом по тому же UART, что и данные
  // файла, и заставляла Z80 перерисовать всё окно. Повтор отсекаем здесь:
  // ход передачи показывает отдельное событие прогресса.
  if (strcmp(shown, lastOperationText) == 0) {
    return;
  }
  snprintf(lastOperationText, sizeof(lastOperationText), "%s", shown);
  eventSink(eventContext, kEventSmbCommand,
            reinterpret_cast<const uint8_t*>(shown),
            static_cast<uint16_t>(used));
}

void SmbServer::Impl::clearTransferProgress(Handle& handle,
                                            bool releaseMemory) {
  handle.progressMode = TransferProgressMode::kNone;
  handle.progressBytes = 0;
  handle.progressRangeCount = 0;
  if (!releaseMemory) {
    return;
  }
  heap_caps_free(handle.progressRanges);
  handle.progressRanges = nullptr;
  handle.progressRangeCapacity = 0;
}

bool SmbServer::Impl::noteTransferProgress(Handle& handle,
                                           TransferProgressMode mode,
                                           uint32_t offset,
                                           uint32_t length) {
  if (length == 0 || mode == TransferProgressMode::kNone ||
      offset > UINT32_MAX - length) {
    return length == 0;
  }
  if (handle.progressMode != mode) {
    clearTransferProgress(handle, false);
    handle.progressMode = mode;
  }

  const uint32_t end = offset + length;
  size_t first = 0;
  while (first < handle.progressRangeCount &&
         handle.progressRanges[first].end < offset) {
    ++first;
  }

  uint32_t mergedStart = offset;
  uint32_t mergedEnd = end;
  uint64_t removedBytes = 0;
  size_t after = first;
  while (after < handle.progressRangeCount &&
         handle.progressRanges[after].start <= mergedEnd) {
    const TransferRange& current = handle.progressRanges[after];
    if (current.start < mergedStart) {
      mergedStart = current.start;
    }
    if (current.end > mergedEnd) {
      mergedEnd = current.end;
    }
    removedBytes += static_cast<uint64_t>(current.end - current.start);
    ++after;
  }

  const size_t removedRanges = after - first;
  const size_t required = handle.progressRangeCount - removedRanges + 1;
  if (required > handle.progressRangeCapacity) {
    size_t capacity = handle.progressRangeCapacity == 0
                          ? static_cast<size_t>(8)
                          : handle.progressRangeCapacity;
    while (capacity < required && capacity <= SIZE_MAX / 2) {
      capacity *= 2;
    }
    if (capacity < required || capacity > SIZE_MAX / sizeof(TransferRange)) {
      return false;
    }
    TransferRange* ranges = static_cast<TransferRange*>(heap_caps_malloc(
        capacity * sizeof(TransferRange), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (ranges == nullptr) {
      ranges = static_cast<TransferRange*>(heap_caps_malloc(
          capacity * sizeof(TransferRange), MALLOC_CAP_8BIT));
    }
    if (ranges == nullptr) {
      return false;
    }
    if (handle.progressRangeCount != 0) {
      memcpy(ranges, handle.progressRanges,
             handle.progressRangeCount * sizeof(TransferRange));
    }
    heap_caps_free(handle.progressRanges);
    handle.progressRanges = ranges;
    handle.progressRangeCapacity = capacity;
  }

  if (after < handle.progressRangeCount) {
    memmove(handle.progressRanges + first + 1,
            handle.progressRanges + after,
            (handle.progressRangeCount - after) * sizeof(TransferRange));
  }
  handle.progressRanges[first].start = mergedStart;
  handle.progressRanges[first].end = mergedEnd;
  handle.progressRangeCount = required;

  const uint64_t covered = static_cast<uint64_t>(handle.progressBytes) -
                           removedBytes +
                           static_cast<uint64_t>(mergedEnd - mergedStart);
  handle.progressBytes =
      covered > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(covered);
  return true;
}

void SmbServer::Impl::clipTransferProgress(Handle& handle, uint32_t size) {
  uint64_t covered = 0;
  size_t kept = 0;
  for (size_t index = 0; index < handle.progressRangeCount; ++index) {
    TransferRange current = handle.progressRanges[index];
    if (current.start >= size) {
      break;
    }
    if (current.end > size) {
      current.end = size;
    }
    handle.progressRanges[kept++] = current;
    covered += static_cast<uint64_t>(current.end - current.start);
  }
  handle.progressRangeCount = kept;
  handle.progressBytes =
      covered > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(covered);
}

// Человекочитаемый размер с одним знаком после запятой. Делить и печатать на
// стороне ESP дешевле, чем городить 32-битную арифметику в плагине Z80.
static void formatTransferSize(uint32_t bytes, char* out, size_t capacity) {
  const uint32_t kMega = 1024UL * 1024UL;
  const uint32_t kKilo = 1024UL;
  if (bytes >= kMega) {
    // Десятая доля МиБ менялась лишь раз примерно на каждые 102 КиБ. При
    // реальной скорости FILEX это выглядело как застывший счётчик, даже когда
    // физические окна продолжали идти. Сотая доля различает каждый 16-КиБ шаг.
    snprintf(out, capacity, "%lu.%02luM",
             static_cast<unsigned long>(bytes / kMega),
             static_cast<unsigned long>(((bytes % kMega) * 100UL) / kMega));
  } else if (bytes >= kKilo) {
    snprintf(out, capacity, "%lu.%luK",
             static_cast<unsigned long>(bytes / kKilo),
             static_cast<unsigned long>(((bytes % kKilo) * 10UL) / kKilo));
  } else {
    snprintf(out, capacity, "%luB", static_cast<unsigned long>(bytes));
  }
}

void SmbServer::Impl::sendProgress(const Handle& handle, const char* operation,
                                   uint32_t position, bool force) {
  if (eventSink == nullptr || operation == nullptr || handle.directory) {
    return;
  }
  const uint32_t now = millis();
  // Без ограничения частоты индикация снова начнёт есть полосу UART у самих
  // данных: одно событие на блок — это тысячи пакетов на файл.
  if (!force &&
      static_cast<uint32_t>(now - progressStampMs) < kProgressIntervalMs) {
    return;
  }
  char doneText[16] = {};
  char totalText[16] = {};
  formatTransferSize(position, doneText, sizeof(doneText));
  formatTransferSize(visibleSize(handle), totalText, sizeof(totalText));
  char shown[32] = {};
  const int written = snprintf(shown, sizeof(shown), "%s %s/%s", operation,
                               doneText, totalText);
  if (written <= 0) {
    return;
  }
  size_t used = static_cast<size_t>(written);
  if (used >= sizeof(shown)) {
    used = sizeof(shown) - 1;
  }
  progressStampMs = now;
  if (strcmp(shown, lastProgressText) == 0) {
    return;
  }
  snprintf(lastProgressText, sizeof(lastProgressText), "%s", shown);
  eventSink(eventContext, kEventSmbProgress,
            reinterpret_cast<const uint8_t*>(shown),
            static_cast<uint16_t>(used));
}

void SmbServer::Impl::resetProgress() {
  lastProgressText[0] = 0;
  progressStampMs = 0;
  if (eventSink != nullptr) {
    // Пустая строка гасит поле, чтобы после конца передачи в окне не висел
    // застывший счётчик предыдущего файла.
    eventSink(eventContext, kEventSmbProgress, nullptr, 0);
  }
}

void SmbServer::Impl::startDiscovery() {
  discoveryRunning = strlen(hostname) <= 15 && udp.begin(137) == 1;
  llmnrRunning = udpLlmnr.beginMulticast(IPAddress(224, 0, 0, 252), 5355) == 1;
  // NBNS отвечает за старое разрешение имени, LLMNR — за современное разрешение
  // имени в Windows 10/11, а WS-Discovery — за плитку компьютера в проводнике.
  const bool wsdStarted = wsDiscovery.begin(hostname, workgroup, discoveryId,
                                             ZIFI_BUILD_VERSION);
  // Запоминаем адрес, на котором подняты сокеты: сравнение с ним и есть
  // признак того, что DHCP выдал плате новый адрес.
  announcedAddress = static_cast<uint32_t>(WiFi.localIP());
  diagnosticLogEvent("DISCOVERY start nbns=%u llmnr=%u wsd=%u ip=%lu",
                     discoveryRunning ? 1U : 0U,
                     llmnrRunning ? 1U : 0U,
                     wsdStarted ? 1U : 0U,
                     static_cast<unsigned long>(announcedAddress));
}

// Сокеты обнаружения привязаны к адресу, действовавшему на момент запуска.
// После смены адреса они перестают слышать широковещательные и многоадресные
// запросы, и отвечать на разрешение имени становится некому. Короткий TTL в
// ответах NBNS и LLMNR тут не помогает: Windows спросит заново, а нас на линии
// уже не будет — и имя останется указывать в пустоту, хотя сервер жив.
bool SmbServer::Impl::addressChanged() {
  const uint32_t current = static_cast<uint32_t>(WiFi.localIP());
  // Нулевой адрес означает, что Wi-Fi сейчас без адреса вовсе. Дёргать контур
  // обнаружения в этот момент бессмысленно: дождёмся нового адреса.
  return current != 0 && current != announcedAddress;
}

void SmbServer::Impl::stopDiscovery() {
  if (discoveryRunning || llmnrRunning || wsDiscovery.running()) {
    diagnosticLogEvent("DISCOVERY stop nbns=%u llmnr=%u wsd=%u",
                       discoveryRunning ? 1U : 0U,
                       llmnrRunning ? 1U : 0U,
                       wsDiscovery.running() ? 1U : 0U);
  }
  wsDiscovery.stop();
  if (discoveryRunning) {
    udp.stop();
  }
  if (llmnrRunning) {
    udpLlmnr.stop();
  }
  discoveryRunning = false;
  llmnrRunning = false;
}

void SmbServer::Impl::pollDiscovery() {
  if (!runningFlag) {
    return;
  }
  if (addressChanged()) {
    // Поднимаем контур заново целиком: WS-Discovery успевает попрощаться и
    // представиться новым экземпляром, чтобы плитка в «Сети» не осталась с
    // прежним адресом, а сокеты NBNS и LLMNR привязываются к новому.
    diagnosticLogEvent("DISCOVERY address-changed old=%lu new=%lu",
                       static_cast<unsigned long>(announcedAddress),
                       static_cast<unsigned long>(
                           static_cast<uint32_t>(WiFi.localIP())));
    stopDiscovery();
    startDiscovery();
    return;
  }
  wsDiscovery.poll();
  if (discoveryRunning) {
    const int packetLength = udp.parsePacket();
    if (packetLength > 0) {
      const size_t wanted = minimum(static_cast<size_t>(packetLength),
                                    sizeof(nbnsPacket));
      const int received = udp.read(nbnsPacket, wanted);
      while (udp.available() > 0) {
        udp.read();
      }
      if (received > 0) {
        answerNbns(static_cast<size_t>(received));
      }
    }
  }
  if (llmnrRunning) {
    const int packetLength = udpLlmnr.parsePacket();
    if (packetLength > 0) {
      uint8_t buffer[512] = {};
      const size_t wanted = minimum(static_cast<size_t>(packetLength),
                                    sizeof(buffer));
      const int received = udpLlmnr.read(buffer, wanted);
      while (udpLlmnr.available() > 0) {
        udpLlmnr.read();
      }
      if (received > 0) {
        answerLlmnr(buffer, static_cast<size_t>(received));
      }
    }
  }
}

void SmbServer::Impl::answerNbns(size_t length) {
  if (length < 50 || readBe16(nbnsPacket + 4) == 0 ||
      (readBe16(nbnsPacket + 2) & 0x8000) != 0 || nbnsPacket[12] != 32) {
    return;
  }

  uint8_t decoded[16] = {};
  for (size_t index = 0; index < 16; ++index) {
    const uint8_t high = nbnsPacket[13 + index * 2];
    const uint8_t low = nbnsPacket[14 + index * 2];
    if (high < 'A' || high > 'P' || low < 'A' || low > 'P') {
      return;
    }
    decoded[index] = static_cast<uint8_t>(((high - 'A') << 4) | (low - 'A'));
  }

  size_t nameEnd = 45;
  while (nameEnd < length && nbnsPacket[nameEnd] != 0) {
    const size_t labelLength = nbnsPacket[nameEnd];
    if (labelLength == 0 || nameEnd + 1 + labelLength >= length) {
      return;
    }
    nameEnd += 1 + labelLength;
  }
  if (nameEnd + 5 > length || readBe16(nbnsPacket + nameEnd + 1) != 0x0020 ||
      readBe16(nbnsPacket + nameEnd + 3) != 0x0001) {
    return;
  }

  char asked[16] = {};
  size_t askedLength = 15;
  while (askedLength != 0 && decoded[askedLength - 1] == ' ') {
    --askedLength;
  }
  memcpy(asked, decoded, askedLength);
  asked[askedLength] = 0;
  const uint8_t suffix = decoded[15];
  if (!asciiEqualNoCase(asked, hostname) ||
      (suffix != 0x00 && suffix != 0x20)) {
    return;
  }

  uint8_t response[80] = {};
  memcpy(response, nbnsPacket, 2);            // transaction ID
  writeBe16(response + 2, 0x8500);            // ответ, authoritative
  writeBe16(response + 4, 0);                 // вопрос не повторяем
  writeBe16(response + 6, 1);                 // одна запись имени
  size_t output = 12;
  const size_t encodedLength = nameEnd + 1 - 12;
  memcpy(response + output, nbnsPacket + 12, encodedLength);
  output += encodedLength;
  writeBe16(response + output, 0x0020);
  output += 2;
  writeBe16(response + output, 0x0001);
  output += 2;
  writeBe32(response + output, 15);            // Короткий TTL 15 сек для мгновенной адаптации к смене IP
  output += 4;
  writeBe16(response + output, 6);
  output += 2;
  writeBe16(response + output, 0x0000);        // уникальное B-node имя
  output += 2;
  const IPAddress address = WiFi.localIP();
  for (size_t index = 0; index < 4; ++index) {
    response[output++] = address[index];
  }

  udp.beginPacket(udp.remoteIP(), udp.remotePort());
  udp.write(response, output);
  udp.endPacket();
}

void SmbServer::Impl::answerLlmnr(const uint8_t* packet, size_t length) {
  if (packet == nullptr || length < 12) {
    return;
  }
  const uint16_t flags = readBe16(packet + 2);
  const uint16_t qdcount = readBe16(packet + 4);
  // QR bit must be 0 (Query) and opcode 0, qdcount >= 1
  if ((flags & 0x8000) != 0 || qdcount == 0) {
    return;
  }

  size_t offset = 12;
  char queriedName[64] = {};
  size_t queriedLength = 0;
  while (offset < length && packet[offset] != 0) {
    const uint8_t labelLength = packet[offset++];
    if (labelLength == 0 || (labelLength & 0xC0) != 0 ||
        offset + labelLength > length) {
      return;
    }
    if (queriedLength != 0 && queriedLength < sizeof(queriedName) - 1) {
      queriedName[queriedLength++] = '.';
    }
    for (size_t i = 0; i < labelLength; ++i) {
      if (queriedLength < sizeof(queriedName) - 1) {
        queriedName[queriedLength++] = static_cast<char>(packet[offset + i]);
      }
    }
    offset += labelLength;
  }
  queriedName[queriedLength] = 0;
  if (offset >= length || packet[offset] != 0) {
    return;
  }
  ++offset;  // skip null byte

  if (offset + 4 > length) {
    return;
  }
  const uint16_t qtype = readBe16(packet + offset);
  const uint16_t qclass = readBe16(packet + offset + 2);
  const size_t questionSectionLength = (offset + 4) - 12;

  // Answer IPv4 (A record = 1) or ANY (255)
  if ((qtype != 1 && qtype != 255) || (qclass != 1 && qclass != 0x8001)) {
    return;
  }

  // Match single-label hostname (e.g. "ZX-EVO")
  if (!asciiEqualNoCase(queriedName, hostname)) {
    return;
  }

  // Build Response Packet
  uint8_t response[128] = {};
  memcpy(response, packet, 2);               // Transaction ID
  writeBe16(response + 2, 0x8400);            // Response (QR=1), Authoritative (AA=1)
  writeBe16(response + 4, 1);                 // 1 Question
  writeBe16(response + 6, 1);                 // 1 Answer
  writeBe16(response + 8, 0);                 // 0 Authority RRs
  writeBe16(response + 10, 0);                // 0 Additional RRs

  // Copy Question Section
  memcpy(response + 12, packet + 12, questionSectionLength);
  size_t respLen = 12 + questionSectionLength;

  // Answer Section
  writeBe16(response + respLen, 0xC00C);      // Pointer to question name at offset 12
  writeBe16(response + respLen + 2, 0x0001);  // Type: A (IPv4)
  writeBe16(response + respLen + 4, 0x0001);  // Class: IN
  writeBe32(response + respLen + 6, 15);      // TTL: 15 seconds
  writeBe16(response + respLen + 10, 4);      // RDLENGTH: 4 bytes
  const IPAddress localIp = WiFi.localIP();
  for (size_t i = 0; i < 4; ++i) {
    response[respLen + 12 + i] = localIp[i];
  }
  respLen += 16;

  udpLlmnr.beginPacket(udpLlmnr.remoteIP(), udpLlmnr.remotePort());
  udpLlmnr.write(response, respLen);
  udpLlmnr.endPacket();
}

bool SmbServer::Impl::requestVfs(VfsOperation operation, const char* path,
                                 uint32_t value, VfsResult& result,
                                 uint32_t timeoutMs) {
  memset(&result, 0, sizeof(result));
  if (!bridge.submit(operation, path, value)) {
    snprintf(lastVfsError, sizeof(lastVfsError), "bridge-busy");
    diagnosticLogEvent(
        "SMB vfs-submit-fail op=%u error=%s holder=%u held=%lu ready=%u",
        static_cast<unsigned>(operation), lastVfsError,
        static_cast<unsigned>(bridge.pendingOperation()),
        static_cast<unsigned long>(millis() - bridge.pendingSinceMs()),
        bridge.ready() ? 1U : 0U);
    return false;
  }
  const uint32_t started = millis();
  while (static_cast<uint32_t>(millis() - started) < timeoutMs) {
    if (bridge.takeResult(result)) {
      if (!result.success) {
        snprintf(lastVfsError, sizeof(lastVfsError), "%s", result.error);
        diagnosticLogEvent("SMB vfs-fail op=%u status=%u moved=%lu error=%s",
                           static_cast<unsigned>(operation),
                           static_cast<unsigned>(result.status),
                           static_cast<unsigned long>(result.transferred),
                           lastVfsError);
        return false;
      }
      snprintf(lastVfsError, sizeof(lastVfsError), "none");
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  snprintf(lastVfsError, sizeof(lastVfsError), "bridge-timeout-%u",
           static_cast<unsigned>(operation));
  diagnosticLogEvent("SMB vfs-timeout op=%u", static_cast<unsigned>(operation));
  return false;
}

bool SmbServer::Impl::requestVfsAt(VfsOperation operation, uint32_t offset,
                                   uint32_t length, VfsResult& result,
                                   uint32_t timeoutMs) {
  memset(&result, 0, sizeof(result));
  if (!bridge.submitAt(operation, offset, length)) {
    snprintf(lastVfsError, sizeof(lastVfsError), "bridge-busy");
    diagnosticLogEvent(
        "SMB vfs-at-submit-fail op=%u off=%lu len=%lu error=%s holder=%u "
        "held=%lu ready=%u",
        static_cast<unsigned>(operation), static_cast<unsigned long>(offset),
        static_cast<unsigned long>(length), lastVfsError,
        static_cast<unsigned>(bridge.pendingOperation()),
        static_cast<unsigned long>(millis() - bridge.pendingSinceMs()),
        bridge.ready() ? 1U : 0U);
    return false;
  }
  const uint32_t started = millis();
  while (static_cast<uint32_t>(millis() - started) < timeoutMs) {
    if (bridge.takeResult(result)) {
      if (!result.success) {
        snprintf(lastVfsError, sizeof(lastVfsError), "%s", result.error);
        diagnosticLogEvent(
            "SMB vfs-at-fail op=%u off=%lu len=%lu status=%u moved=%lu error=%s",
            static_cast<unsigned>(operation), static_cast<unsigned long>(offset),
            static_cast<unsigned long>(length),
            static_cast<unsigned>(result.status),
            static_cast<unsigned long>(result.transferred), lastVfsError);
        return false;
      }
      snprintf(lastVfsError, sizeof(lastVfsError), "none");
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  snprintf(lastVfsError, sizeof(lastVfsError), "bridge-timeout-at-%u",
           static_cast<unsigned>(operation));
  diagnosticLogEvent("SMB vfs-at-timeout op=%u off=%lu len=%lu",
                     static_cast<unsigned>(operation),
                     static_cast<unsigned long>(offset),
                     static_cast<unsigned long>(length));
  return false;
}

bool SmbServer::Impl::requestRename(const char* oldPath, const char* newName,
                                    bool directory, VfsResult& result) {
  memset(&result, 0, sizeof(result));
  if (!bridge.submitRename(oldPath, newName, directory)) {
    snprintf(lastVfsError, sizeof(lastVfsError), "bridge-busy");
    return false;
  }
  const uint32_t started = millis();
  while (static_cast<uint32_t>(millis() - started) < kMutateVfsTimeoutMs) {
    if (bridge.takeResult(result)) {
      if (!result.success) {
        snprintf(lastVfsError, sizeof(lastVfsError), "%s", result.error);
        return false;
      }
      snprintf(lastVfsError, sizeof(lastVfsError), "none");
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  snprintf(lastVfsError, sizeof(lastVfsError), "bridge-timeout-rename");
  return false;
}

bool SmbServer::Impl::requestMoveRename(const char* oldPath,
                                        const char* newPath, bool directory,
                                        bool replace, VfsResult& result) {
  memset(&result, 0, sizeof(result));
  if (!bridge.submitMoveRename(oldPath, newPath, directory, replace)) {
    snprintf(lastVfsError, sizeof(lastVfsError), "bridge-busy");
    return false;
  }
  const uint32_t started = millis();
  while (static_cast<uint32_t>(millis() - started) < kMutateVfsTimeoutMs) {
    if (bridge.takeResult(result)) {
      if (!result.success) {
        snprintf(lastVfsError, sizeof(lastVfsError), "%s", result.error);
        return false;
      }
      snprintf(lastVfsError, sizeof(lastVfsError), "none");
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  snprintf(lastVfsError, sizeof(lastVfsError), "bridge-timeout-move");
  return false;
}

bool SmbServer::Impl::requestMetadata(const VfsMetadata& metadata,
                                      VfsResult& result) {
  memset(&result, 0, sizeof(result));
  if (!bridge.submitMetadata(metadata)) {
    snprintf(lastVfsError, sizeof(lastVfsError), "bridge-busy");
    return false;
  }
  const uint32_t started = millis();
  while (static_cast<uint32_t>(millis() - started) < kMutateVfsTimeoutMs) {
    if (bridge.takeResult(result)) {
      if (!result.success) {
        snprintf(lastVfsError, sizeof(lastVfsError), "%s", result.error);
        return false;
      }
      snprintf(lastVfsError, sizeof(lastVfsError), "none");
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  snprintf(lastVfsError, sizeof(lastVfsError), "bridge-timeout-metadata");
  return false;
}

bool SmbServer::Impl::closeActive(bool commit) {
  if (activeSlot < 0 || activeMode == ActiveMode::kNone) {
    return true;
  }
  // Синхронный CREATE/RENAME может прийти от второго клиента, пока async READ
  // другого владельца держит мост или ещё не забрал данные из кольца. Раньше
  // мы сначала забывали activeSlot, затем получали bridge-busy на CLOSE — и
  // теряли единственную запись о реально открытом FILEX-файле. Оставляем всё
  // состояние без изменений; завершивший async-путь закроет файл сам.
  if (activeAsyncRead >= 0 || activeAsyncWrite >= 0 ||
      bridge.requestPending()) {
    snprintf(lastVfsError, sizeof(lastVfsError), "bridge-busy");
    return false;
  }
  const int slot = activeSlot;
  const ActiveMode mode = activeMode;
  activeSlot = -1;
  activeMode = ActiveMode::kNone;
  activeLogicalOffset = 0;
  activeVfsOffset = 0;
  activeRandomWrite = false;
  if (mode == ActiveMode::kDirectory) {
    return true;
  }

  VfsResult result;
  const VfsOperation operation = commit ? VfsOperation::kCloseCommit
                                        : VfsOperation::kCloseAbort;
  // Долгий таймаут оправдан только закрытием записи: там Z80 дописывает FAT и
  // каталог, и это законно занимает минуты. Закрытие чтения ничего не сбрасывает
  // на карту, а трёхминутное ожидание здесь замораживает весь сервер — Windows
  // за это время успевает разорвать сессию и бросить копирование.
  const uint32_t timeout = mode == ActiveMode::kWrite ? kMutateVfsTimeoutMs
                                                      : kNormalVfsTimeoutMs;
  const bool closed = requestVfs(operation, nullptr, 0, result, timeout);
  if (!closed && commit) {
    requestVfs(VfsOperation::kCloseAbort, nullptr, 0, result, timeout);
  }
  if (mode == ActiveMode::kWrite && slot >= 0 &&
      static_cast<size_t>(slot) < kHandleCount) {
    handles[slot].failed = handles[slot].failed || !closed;
  }
  return closed;
}

bool SmbServer::Impl::resetBuffers() {
  VfsResult result;
  return requestVfs(VfsOperation::kResetBuffers, nullptr, 0, result,
                    kNormalVfsTimeoutMs);
}

bool SmbServer::Impl::statPath(const char* path, VfsResult& result) {
  memset(&result, 0, sizeof(result));
  // Быстрый путь по уже открытым дескрипторам. Открывая файл, Проводник шлёт
  // подряд четыре-пять CREATE: проверка атрибутов, расширенные атрибуты,
  // собственно чтение, плюс превью и проверка типа из отдельных потоков
  // оболочки. Всё, что им нужно, у сервера уже есть в памяти, поэтому
  // обращаться за этим к Z80 незачем — и, главное, нельзя: такой запрос
  // приходит поверх идущего чтения и конкурирует с ним за единственный
  // канал к SD.
  for (size_t index = 0; index < kHandleCount; ++index) {
    const Handle& handle = handles[index];
    if (!handle.used || !asciiEqualNoCase(handle.path, path)) {
      continue;
    }
    result.success = true;
    result.isDirectory = handle.directory;
    result.size = visibleSize(handle);
    snprintf(lastVfsError, sizeof(lastVfsError), "none");
    return true;
  }

  char parent[kMaxPath + 1];
  char name[kMaxPath + 1];
  const bool hasParent = splitParent(path, parent, name);
  if (hasParent) {
    // Частичный снимок ещё нельзя публиковать как полный кэш: отсутствие имени
    // в нём ничего не доказывает. Но положительный результат последнего
    // QUERY_DIRECTORY уже является точным STAT этой записи и безопасно
    // обслуживает параллельный CREATE Проводника полностью из памяти.
    for (const Handle& directoryHandle : handles) {
      if (!directoryHandle.used || !directoryHandle.directory ||
          !directoryHandle.directoryLastValid ||
          !asciiEqualNoCase(directoryHandle.path, parent) ||
          !asciiEqualNoCase(directoryHandle.directoryLastName, name)) {
        continue;
      }
      result.success = true;
      result.isDirectory = directoryHandle.directoryLastIsDirectory;
      result.size = directoryHandle.directoryLastSize;
      snprintf(result.name, sizeof(result.name), "%s",
               directoryHandle.directoryLastName);
      snprintf(lastVfsError, sizeof(lastVfsError), "none");
      return true;
    }
  }
  if (hasParent && directoryCache.contains(parent)) {
    DirectoryCache::EntryView cached;
    if (directoryCache.findEntry(parent, name, cached)) {
      result.success = true;
      result.isDirectory = cached.isDirectory;
      result.size = cached.size;
      snprintf(result.name, sizeof(result.name), "%s", cached.name);
      snprintf(lastVfsError, sizeof(lastVfsError), "none");
      return true;
    }
    snprintf(lastVfsError, sizeof(lastVfsError), "not-found");
    return false;
  }
  // Если дескриптор и снимок каталога не дали попадания — проверяем на Z80.
  // Трогать файловый контекст во время активной передачи нельзя: closeActive
  // закрыл бы открытый файл посреди окна. Не блокируем фоновые потоки Проводника.
  if (asyncReadCount != 0 || asyncWriteCount != 0 || activeAsyncRead >= 0 ||
      activeAsyncWrite >= 0) {
    snprintf(lastVfsError, sizeof(lastVfsError), "not-found");
    return false;
  }
  if (!closeActive(true)) {
    return false;
  }
  // STAT ничего на карте не меняет. Сброс родительского каталога здесь
  // обнулял directoryIndex всех открытых Проводником handle: следующий
  // QUERY_DIRECTORY начинал тот же каталог заново и показывал каждое имя
  // дважды. После закрытия общего физического курсора activateDirectory()
  // сам восстановит ровно сохранённую логическую позицию.
  return requestVfs(VfsOperation::kStat, path, 0, result,
                    kNormalVfsTimeoutMs);
}

bool SmbServer::Impl::createEmptyFile(const char* path) {
  // Правка тома делает копию файла в PSRAM недостоверной: отдать по сети
  // устаревшие байты хуже, чем прочитать файл заново.
  dropFileCache();
  if (!closeActive(true) || !resetBuffers()) {
    return false;
  }
  VfsResult result;
  if (!requestVfs(VfsOperation::kOpenWrite, path, 0, result,
                  kMutateVfsTimeoutMs)) {
    return false;
  }
  const bool created = requestVfs(VfsOperation::kCloseCommit, nullptr, 0,
                                  result, kMutateVfsTimeoutMs);
  if (created) {
    invalidateFsInfo();
    invalidateParent(path);
  }
  return created;
}

bool SmbServer::Impl::removePath(const char* path) {
  // Правка тома делает копию файла в PSRAM недостоверной: отдать по сети
  // устаревшие байты хуже, чем прочитать файл заново.
  dropFileCache();
  if (!closeActive(true)) {
    return false;
  }
  // Размер файла запоминаем до удаления: по нему освобождённые кластеры
  // возвращаются в кэш арифметикой. Иначе каждое удаление гасило сведения о
  // томе, и следующий же запрос Проводника заставлял Z80 просканировать всю
  // активную FAT — при чистке каталога это происходило на каждом файле.
  uint32_t releasedSize = 0;
  bool releasedKnown = false;
  char parent[kMaxPath + 1];
  char name[kMaxPath + 1];
  DirectoryCache::EntryView cached;
  if (splitParent(path, parent, name) &&
      directoryCache.findEntry(parent, name, cached) && !cached.isDirectory) {
    releasedSize = cached.size;
    releasedKnown = true;
  }
  VfsResult result;
  const bool removed = requestVfs(VfsOperation::kDelete, path, 0, result,
                                  kMutateVfsTimeoutMs);
  if (removed) {
    forgetMetadata(path);
    invalidateFsInfo();
    invalidateParent(path);
    invalidateSubtree(path);
    notifyChange(path, SMB2_NOTIFY_CHANGE_FILE_ACTION_REMOVED,
                 SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_FILE_NAME |
                     SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_DIR_NAME);
  }
  return removed;
}

SmbServer::Impl::CacheLoadResult
SmbServer::Impl::ensureDirectoryCached(const char* path) {
  if (directoryCache.contains(path)) {
    return CacheLoadResult::kReady;
  }
  // Служебная проверка пустоты является только чтением. Если QUERY_DIRECTORY
  // уже строит другой снимок, не уничтожаем его: вызывающий код проверит
  // целевой каталог прямым физическим обходом, а построение продолжится при
  // следующем запросе исходного каталога.
  if (directoryCacheBuildSlot >= 0) {
    return CacheLoadResult::kUnavailable;
  }
  if (!directoryCache.enabled()) {
    return CacheLoadResult::kUnavailable;
  }
  if (directoryCache.isUncacheable(path)) {
    // Этот каталог уже не поместился целиком. Повторять полный обход по UART
    // ради того же отказа нельзя: раньше он выполнялся на каждом обращении.
    snprintf(lastVfsError, sizeof(lastVfsError), "cache-full");
    return CacheLoadResult::kUnavailable;
  }
  if (!closeActive(true)) {
    return CacheLoadResult::kIoError;
  }

  VfsResult result;
  if (!requestVfs(VfsOperation::kOpenDirectory, path, 0, result,
                  kNormalVfsTimeoutMs)) {
    return CacheLoadResult::kIoError;
  }
  if (!directoryCache.beginSnapshot(path)) {
    snprintf(lastVfsError, sizeof(lastVfsError), "cache-no-memory");
    return CacheLoadResult::kUnavailable;
  }

  while (true) {
    if (!requestVfs(VfsOperation::kReadDirectory, nullptr, 0, result,
                    kNormalVfsTimeoutMs)) {
      directoryCache.abortSnapshot();
      return CacheLoadResult::kIoError;
    }
    if (result.atEnd) {
      if (!directoryCache.finishSnapshot()) {
        directoryCache.abortSnapshot();
        return CacheLoadResult::kIoError;
      }
      sendOperation("CACHE", path);
      return CacheLoadResult::kReady;
    }
    if (!directoryCache.append(result.isDirectory, result.size,
                               result.name)) {
      // Один слишком большой каталог не должен ломать SMB. Освобождаем его
      // неполный снимок и ниже используем прежнее потоковое чтение с Z80.
      // Заглушка помнит решение, чтобы следующее обращение сразу пошло
      // потоковым путём вместо ещё одного бесполезного полного обхода.
      directoryCache.abortSnapshot();
      directoryCache.markUncacheable(path);
      snprintf(lastVfsError, sizeof(lastVfsError), "cache-full");
      return CacheLoadResult::kUnavailable;
    }
  }
}

bool SmbServer::Impl::beginDirectoryCacheBuild(int slot, Handle& handle) {
  if (slot < 0 || static_cast<size_t>(slot) >= kHandleCount ||
      directoryCacheBuildSlot >= 0 || handle.directoryIndex != 0 ||
      handle.directoryCacheUnavailable || !directoryCache.enabled() ||
      directoryCache.contains(handle.path) ||
      directoryCache.isUncacheable(handle.path)) {
    return false;
  }
  if (!directoryCache.beginSnapshot(handle.path)) {
    handle.directoryCacheUnavailable = true;
    return false;
  }
  directoryCacheBuildSlot = slot;
  directoryCacheBuildGeneration = handle.generation;
  diagnosticLogEvent("SMB directory-cache-build path=%s slot=%d",
                     handle.path, slot);
  return true;
}

bool SmbServer::Impl::appendDirectoryCacheBuild(
    int slot, Handle& handle, const VfsResult& result) {
  if (directoryCacheBuildSlot != slot ||
      directoryCacheBuildGeneration != handle.generation) {
    return true;
  }
  if (directoryCache.append(result.isDirectory, result.size, result.name)) {
    return true;
  }

  // Снимок обязан быть полным. При нехватке PSRAM удаляем частичный список и
  // запоминаем потоковый режим, чтобы следующий Open не повторял бесполезную
  // попытку накопить тот же слишком большой каталог.
  char failedPath[kMaxPath + 1] = {};
  snprintf(failedPath, sizeof(failedPath), "%s", handle.path);
  abortDirectoryCacheBuild();
  handle.directoryCacheUnavailable = true;
  directoryCache.markUncacheable(failedPath);
  snprintf(lastVfsError, sizeof(lastVfsError), "cache-full");
  return false;
}

void SmbServer::Impl::finishDirectoryCacheBuild(int slot, Handle& handle) {
  if (directoryCacheBuildSlot != slot ||
      directoryCacheBuildGeneration != handle.generation) {
    return;
  }
  const bool finished = directoryCache.finishSnapshot();
  directoryCacheBuildSlot = -1;
  directoryCacheBuildGeneration = 0;
  if (finished) {
    sendOperation("CACHE", handle.path);
  } else {
    handle.directoryCacheUnavailable = true;
  }
}

void SmbServer::Impl::abortDirectoryCacheBuild() {
  if (directoryCacheBuildSlot >= 0) {
    directoryCache.abortSnapshot();
  }
  directoryCacheBuildSlot = -1;
  directoryCacheBuildGeneration = 0;
}

SmbServer::Impl::DirectoryContents
SmbServer::Impl::checkDirectoryContents(const char* path) {
  const CacheLoadResult cacheResult = ensureDirectoryCached(path);
  if (cacheResult == CacheLoadResult::kReady) {
    DirectoryCache::Cursor cursor;
    if (!directoryCache.openCursor(path, cursor)) {
      return DirectoryContents::kIoError;
    }
    DirectoryCache::EntryView entry;
    while (directoryCache.next(cursor, entry)) {
      if (!asciiEqualNoCase(entry.name, ".") &&
          !asciiEqualNoCase(entry.name, "..")) {
        return DirectoryContents::kNotEmpty;
      }
    }
    return DirectoryContents::kEmpty;
  }
  if (cacheResult == CacheLoadResult::kIoError || !closeActive(true)) {
    return DirectoryContents::kIoError;
  }

  // Без PSRAM, при переполненном кэше или во время построения другого снимка
  // проверяем каталог напрямую. Это чтение не инвалидирует и не прерывает
  // кэш; Z80 отдаёт конец каталога успешным результатом с atEnd=true.
  VfsResult result;
  if (!requestVfs(VfsOperation::kOpenDirectory, path, 0, result,
                  kNormalVfsTimeoutMs)) {
    return DirectoryContents::kIoError;
  }
  while (true) {
    if (!requestVfs(VfsOperation::kReadDirectory, nullptr, 0, result,
                    kNormalVfsTimeoutMs)) {
      return DirectoryContents::kIoError;
    }
    if (result.atEnd) {
      return DirectoryContents::kEmpty;
    }
    if (!asciiEqualNoCase(result.name, ".") &&
        !asciiEqualNoCase(result.name, "..")) {
      return DirectoryContents::kNotEmpty;
    }
  }
}

void SmbServer::Impl::invalidateDirectory(const char* path) {
  if (path == nullptr) {
    return;
  }
  // Успешная правка одного каталога не должна выбрасывать снимок другого.
  // Частичный снимок ещё не опубликован, но его также прерываем только тогда,
  // когда запись действительно изменила перечисляемый каталог.
  if (directoryCacheBuildSlot >= 0 &&
      static_cast<size_t>(directoryCacheBuildSlot) < kHandleCount) {
    Handle& building = handles[directoryCacheBuildSlot];
    if (!building.used ||
        building.generation != directoryCacheBuildGeneration ||
        asciiEqualNoCase(building.path, path)) {
      abortDirectoryCacheBuild();
    }
  }
  // Сведения о томе здесь намеренно не сбрасываются: рост и удаление файлов
  // учитываются арифметикой по размеру кластера, а операции, объём которых
  // посчитать нельзя, гасят кэш сами.
  directoryCache.invalidate(path);
  for (size_t index = 0; index < kHandleCount; ++index) {
    Handle& handle = handles[index];
    if (!handle.used || !handle.directory ||
        !asciiEqualNoCase(handle.path, path)) {
      continue;
    }
    handle.directoryIndex = 0;
    handle.directoryEnded = false;
    handle.directoryLastValid = false;
    handle.directoryCursor = {};
    if (activeSlot == static_cast<int>(index) &&
        activeMode == ActiveMode::kDirectory) {
      activeSlot = -1;
      activeMode = ActiveMode::kNone;
    }
  }
}

void SmbServer::Impl::invalidateSubtree(const char* path) {
  // Удаление или перенос дерева затрагивает только снимки внутри этого дерева.
  // Кэш соседних каталогов продолжает жить без таймера и повторного UART-чтения.
  if (path != nullptr && directoryCacheBuildSlot >= 0 &&
      static_cast<size_t>(directoryCacheBuildSlot) < kHandleCount) {
    Handle& building = handles[directoryCacheBuildSlot];
    if (!building.used ||
        building.generation != directoryCacheBuildGeneration ||
        asciiPathBelongsTo(building.path, path)) {
      abortDirectoryCacheBuild();
    }
  }
  directoryCache.invalidateSubtree(path);
  // Любая инвалидация повышает revision. Открытые курсоры безопасно заметят
  // это перед следующим разыменованием и заново найдут свой снимок.
}

void SmbServer::Impl::invalidateParent(const char* path) {
  char parent[kMaxPath + 1];
  char name[kMaxPath + 1];
  if (splitParent(path, parent, name)) {
    invalidateDirectory(parent);
  }
}

void SmbServer::Impl::refreshCachedSize(const char* path, uint32_t size) {
  // Только имя и родитель, без копий пути на стеке: эта функция вызывается на
  // каждом окне записи внутри SMB-задачи, а её стек ограничен.
  if (path == nullptr || *path != '/') {
    return;
  }
  const char* name = strrchr(path, '/');
  if (name == nullptr || name[1] == 0) {
    return;
  }
  const size_t parentLength = name == path ? 1 : static_cast<size_t>(name - path);
  directoryCache.updateEntrySizeAt(path, parentLength, name + 1, size);
}

void SmbServer::Impl::invalidateFsInfo() { fsInfoValid = false; }

void SmbServer::Impl::dropFileCache() {
  if (cachedFileData != nullptr) {
    heap_caps_free(cachedFileData);
    cachedFileData = nullptr;
  }
  cachedFilePath[0] = 0;
  cachedFileSize = 0;
  cachedFileFilled = 0;
}

// Завести копию файла в PSRAM. Место берём только при живом запасе: буферы
// сети и снимки каталогов важнее любого ускорения повторного чтения.
void SmbServer::Impl::beginFileCache(const char* path, uint32_t size) {
  dropFileCache();
  if (path == nullptr || size == 0 || size > kFileCacheLimit ||
      strlen(path) > kMaxPath) {
    return;
  }
  const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (free < static_cast<size_t>(size) + kFileCachePsramFloor) {
    return;
  }
  cachedFileData = static_cast<uint8_t*>(
      heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (cachedFileData == nullptr) {
    return;
  }
  snprintf(cachedFilePath, sizeof(cachedFilePath), "%s", path);
  cachedFileSize = size;
  cachedFileFilled = 0;
}

// Копия наполняется только сплошным потоком с начала. Любой пропуск означает,
// что дальше в кэше будет дыра, и такую копию честнее выбросить целиком.
void SmbServer::Impl::appendFileCache(uint32_t offset, const uint8_t* data,
                                      uint32_t length) {
  if (cachedFileData == nullptr || data == nullptr || length == 0) {
    return;
  }
  if (offset != cachedFileFilled || offset + length > cachedFileSize) {
    dropFileCache();
    return;
  }
  memcpy(cachedFileData + offset, data, length);
  cachedFileFilled = offset + length;
}

bool SmbServer::Impl::serveFromFileCache(const char* path, uint32_t size,
                                         uint32_t offset, uint32_t length,
                                         uint8_t* output) const {
  if (cachedFileData == nullptr || path == nullptr || output == nullptr ||
      size != cachedFileSize || !asciiEqualNoCase(cachedFilePath, path)) {
    return false;
  }
  if (length == 0 || offset > cachedFileFilled ||
      offset + length > cachedFileFilled) {
    return false;
  }
  memcpy(output, cachedFileData + offset, length);
  return true;
}

void SmbServer::Impl::noteFileGrowth(uint32_t oldSize, uint32_t newSize) {
  if (!fsInfoValid || newSize == oldSize) {
    return;
  }
  const uint32_t perCluster = static_cast<uint32_t>(cachedFsInfo.bytesPerSector) *
                              cachedFsInfo.sectorsPerCluster;
  if (perCluster == 0) {
    fsInfoValid = false;
    return;
  }
  const uint32_t before = (oldSize + perCluster - 1) / perCluster;
  const uint32_t after = (newSize + perCluster - 1) / perCluster;
  if (after >= before) {
    const uint32_t taken = after - before;
    cachedFsInfo.freeClusters =
        cachedFsInfo.freeClusters > taken ? cachedFsInfo.freeClusters - taken : 0;
    return;
  }
  const uint32_t free = cachedFsInfo.freeClusters + (before - after);
  cachedFsInfo.freeClusters =
      free < cachedFsInfo.freeClusters || free > cachedFsInfo.totalClusters
          ? cachedFsInfo.totalClusters
          : free;
}

void SmbServer::Impl::updateSharedPhysicalSize(const char* path,
                                               uint32_t newSize) {
  if (path == nullptr) {
    return;
  }
  uint32_t oldSize = 0;
  bool found = false;
  for (size_t index = 0; index < kHandleCount; ++index) {
    const Handle& handle = handles[index];
    if (handle.used && asciiEqualNoCase(handle.path, path)) {
      oldSize = found && oldSize > handle.physicalSize ? oldSize
                                                       : handle.physicalSize;
      found = true;
    }
  }
  if (!found) {
    return;
  }
  noteFileGrowth(oldSize, newSize);
  for (size_t index = 0; index < kHandleCount; ++index) {
    Handle& handle = handles[index];
    if (!handle.used || !asciiEqualNoCase(handle.path, path)) {
      continue;
    }
    handle.physicalSize = newSize;
    if (handle.position > newSize) {
      handle.position = newSize;
    }
    clipTransferProgress(handle, newSize);
    if (handle.sizeReserved && newSize >= handle.reservedSize) {
      handle.sizeReserved = false;
      handle.ownsSizeReservation = false;
    }
  }
  refreshCachedSize(path, newSize);
}

bool SmbServer::Impl::loadFsInfo(VfsFsInfo& info, uint8_t& status) {
  status = 0;
  if (fsInfoRefreshRequired) {
    VfsResult invalidateResult = {};
    if (!closeActive(true) ||
        !requestVfs(VfsOperation::kInvalidateFsInfo, nullptr, 0,
                    invalidateResult, kNormalVfsTimeoutMs)) {
      status = invalidateResult.status;
      return false;
    }
    fsInfoRefreshRequired = false;
    fsInfoValid = false;
  }
  if (fsInfoValid) {
    info = cachedFsInfo;
    return true;
  }
  VfsResult result = {};
  if (!closeActive(true) ||
      !requestVfs(VfsOperation::kGetFsInfo, nullptr, 0, result,
                  kNormalVfsTimeoutMs)) {
    status = result.status;
    return false;
  }
  cachedFsInfo = result.fsInfo;
  const uint64_t allocationUnit =
      static_cast<uint64_t>(cachedFsInfo.bytesPerSector) *
      cachedFsInfo.sectorsPerCluster;
  if (allocationUnit == 0 || allocationUnit > UINT32_MAX ||
      cachedFsInfo.totalClusters == 0) {
    snprintf(lastVfsError, sizeof(lastVfsError), "fs-info-geometry");
    return false;
  }
  allocationUnitBytes = static_cast<uint32_t>(allocationUnit);
  if ((cachedFsInfo.flags & 0x04) == 0 ||
      cachedFsInfo.freeClusters > cachedFsInfo.totalClusters) {
    cachedFsInfo.freeClusters = cachedFsInfo.totalClusters / 2;
    cachedFsInfo.flags |= 0x04;
  }
  fsInfoValid = true;
  info = cachedFsInfo;
  return true;
}

bool SmbServer::Impl::ensureAllocationUnit(uint8_t& status) {
  if (allocationUnitBytes != 0) {
    status = 0;
    return true;
  }
  VfsFsInfo info = {};
  return loadFsInfo(info, status) && allocationUnitBytes != 0;
}

uint64_t SmbServer::Impl::reportedAllocationSize(
    uint32_t physicalSize, bool directory) const {
  // FAT32 не поддерживает sparse-файлы. Для обычного файла занятое место —
  // физическая длина, округлённая до кластера; каталогам MS-FSCC/Samba
  // возвращают ноль. Логический SET_EOF reserve сюда не входит, пока кластеры
  // действительно не материализованы на SD.
  return directory
             ? 0
             : roundUpAllocationSize(physicalSize, allocationUnitBytes);
}

bool SmbServer::Impl::normalizePath(
    const char* input, char output[kMaxPath + 1]) const {
  if (input == nullptr || output == nullptr) {
    return false;
  }
  output[0] = '/';
  output[1] = 0;
  size_t used = 1;
  const char* cursor = input;
  while (*cursor == '/' || *cursor == '\\') {
    ++cursor;
  }
  while (*cursor != 0) {
    const char* component = cursor;
    size_t componentLength = 0;
    while (cursor[componentLength] != 0 && cursor[componentLength] != '/' &&
           cursor[componentLength] != '\\') {
      ++componentLength;
    }
    // Отсекаем суффикс главного потока NTFS "::$DATA" / ":$DATA"
    size_t effectiveLength = componentLength;
    if (effectiveLength >= 7 &&
        asciiEqualNoCase(component + effectiveLength - 7, "::$DATA")) {
      effectiveLength -= 7;
    } else if (effectiveLength >= 6 &&
               asciiEqualNoCase(component + effectiveLength - 6, ":$DATA")) {
      effectiveLength -= 6;
    }
    for (size_t i = 0; i < effectiveLength; ++i) {
      const unsigned char value = static_cast<unsigned char>(component[i]);
      if (value < 32 || value == ':' || value == '|' || value == '<' ||
          value == '>' || value == '"') {
        return false;
      }
    }
    if (effectiveLength == 1 && component[0] == '.') {
      // Одиночная точка ничего не меняет.
    } else if (effectiveLength == 2 && component[0] == '.' &&
               component[1] == '.') {
      // Не разрешаем выход выше корня общей папки.
      return false;
    } else if (effectiveLength != 0) {
      if (used != 1) {
        if (used >= kMaxPath) {
          return false;
        }
        output[used++] = '/';
      }
      if (effectiveLength > kMaxPath - used) {
        return false;
      }
      memcpy(output + used, component, effectiveLength);
      used += effectiveLength;
      output[used] = 0;
    }
    cursor += componentLength;
    while (*cursor == '/' || *cursor == '\\') {
      ++cursor;
    }
  }
  return true;
}

bool SmbServer::Impl::splitParent(const char* path,
                                  char parent[kMaxPath + 1],
                                  char name[kMaxPath + 1]) const {
  if (path == nullptr || strcmp(path, "/") == 0) {
    return false;
  }
  const char* slash = strrchr(path, '/');
  if (slash == nullptr || slash[1] == 0) {
    return false;
  }
  const size_t parentLength = slash == path ? 1 : static_cast<size_t>(slash - path);
  if (parentLength > kMaxPath || strlen(slash + 1) > kMaxPath) {
    return false;
  }
  memcpy(parent, path, parentLength);
  parent[parentLength] = 0;
  snprintf(name, kMaxPath + 1, "%s", slash + 1);
  return true;
}

bool SmbServer::Impl::parseShare(const smb2_tree_connect_request* request,
                                 char output[32]) const {
  if (request == nullptr || request->path == nullptr ||
      request->path_length < 2) {
    return false;
  }
  size_t used = 0;
  const size_t characters = request->path_length / 2;
  for (size_t index = 0; index < characters; ++index) {
    const uint16_t value = request->path[index];
    if (value == 0) {
      continue;
    }
    if (value == '\\' || value == '/') {
      used = 0;
      continue;
    }
    if (value > 0x7F || used >= 31) {
      return false;
    }
    output[used++] = static_cast<char>(value);
  }
  output[used] = 0;
  return used != 0;
}

bool SmbServer::Impl::makeInfoName(const Handle& handle) {
  size_t length = strlen(handle.path);
  if (length + 1 > sizeof(infoName)) {
    return false;
  }
  if (strcmp(handle.path, "/") == 0) {
    infoName[0] = '\\';
    infoName[1] = 0;
    return true;
  }
  for (size_t index = 0; index <= length; ++index) {
    infoName[index] = handle.path[index] == '/' ? '\\' : handle.path[index];
  }
  return true;
}

int SmbServer::Impl::allocateHandle(smb2_context* owner, uint32_t treeId) {
  if (owner == nullptr || treeId == 0) {
    return -1;
  }
  for (size_t index = 0; index < kHandleCount; ++index) {
    if (handles[index].used) {
      continue;
    }
    Handle& handle = handles[index];
    clearTransferProgress(handle, true);
    memset(&handle, 0, sizeof(handle));
    handle.used = true;
    handle.owner = owner;
    handle.treeId = treeId;
    handle.generation = generationCounter++;
    if (generationCounter == 0) {
      generationCounter = 1;
    }
    writeLe32(handle.fileId, kFileIdMagic);
    writeLe32(handle.fileId + 4, handle.generation);
    writeLe32(handle.fileId + 8, static_cast<uint32_t>(index + 1));
    writeLe32(handle.fileId + 12, ~handle.generation);
    snprintf(handle.pattern, sizeof(handle.pattern), "*");
    return static_cast<int>(index);
  }
  return -1;
}

SmbServer::Impl::Handle* SmbServer::Impl::findHandle(
    smb2_context* owner, const uint8_t fileId[SMB2_FD_SIZE], int* slot) {
  if (owner == nullptr || fileId == nullptr) {
    return nullptr;
  }
  int found = -1;
  if (readLe32(fileId) == kFileIdMagic) {
    const uint32_t encodedSlot = readLe32(fileId + 8);
    if (encodedSlot >= 1 && encodedSlot <= kHandleCount) {
      found = static_cast<int>(encodedSlot - 1);
    }
  }
  if (found < 0 || static_cast<size_t>(found) >= kHandleCount ||
      !handles[found].used || handles[found].owner != owner ||
      handles[found].treeId != smb2_get_current_tree_id(owner) ||
      memcmp(handles[found].fileId, fileId, SMB2_FD_SIZE) != 0) {
    return nullptr;
  }
  if (slot != nullptr) {
    *slot = found;
  }
  return &handles[found];
}

void SmbServer::Impl::releaseHandle(int slot) {
  if (slot < 0 || static_cast<size_t>(slot) >= kHandleCount) {
    return;
  }
  if (directoryCacheBuildSlot == slot &&
      directoryCacheBuildGeneration == handles[slot].generation) {
    abortDirectoryCacheBuild();
  }
  if (activeSlot == slot) {
    closeActive(true);
  }
  cancelNotifiesForHandle(handles[slot].owner, slot,
                          handles[slot].generation, SMB2_STATUS_CANCELLED);
  releaseByteRangeLocksForHandle(slot, handles[slot].generation);
  clearTransferProgress(handles[slot], true);
  memset(&handles[slot], 0, sizeof(handles[slot]));
}

uint32_t SmbServer::Impl::visibleSize(const Handle& handle) const {
  return handle.sizeReserved && handle.reservedSize > handle.physicalSize
             ? handle.reservedSize
             : handle.physicalSize;
}

uint64_t SmbServer::Impl::directoryFileId(const char* path) const {
  return appendDirectoryFileId(kDirectoryFileIdOffset, path);
}

uint64_t SmbServer::Impl::directoryChildFileId(
    const char* parentPath, const char* name) const {
  const char* parent = parentPath == nullptr || parentPath[0] == 0
                           ? "/"
                           : parentPath;
  uint64_t hash = appendDirectoryFileId(kDirectoryFileIdOffset, parent);
  const size_t parentLength = strlen(parent);
  if (parentLength == 0 || parent[parentLength - 1] != '/') {
    hash = appendDirectoryFileId(hash, "/");
  }
  return appendDirectoryFileId(hash, name);
}

SmbServer::Impl::ReportedMetadata SmbServer::Impl::reportedMetadata(
    const char* path, bool directory) const {
  ReportedMetadata result;
  result.creationTime = currentFileTime();
  result.lastAccessTime = result.creationTime;
  result.lastWriteTime = result.creationTime;
  result.changeTime = result.creationTime;
  result.attributes = directory ? SMB2_FILE_ATTRIBUTE_DIRECTORY
                                : SMB2_FILE_ATTRIBUTE_ARCHIVE;
  for (const CachedMetadata& cached : metadataCache) {
    if (cached.used && asciiEqualNoCase(cached.path, path)) {
      return cached.value;
    }
  }
  return result;
}

SmbServer::Impl::ReportedMetadata SmbServer::Impl::reportedChildMetadata(
    const char* parentPath, const char* name, bool directory) const {
  char path[kMaxPath + 1] = {};
  const char* parent = parentPath == nullptr || parentPath[0] == 0
                           ? "/"
                           : parentPath;
  const int length = strcmp(parent, "/") == 0
                         ? snprintf(path, sizeof(path), "/%s", name)
                         : snprintf(path, sizeof(path), "%s/%s", parent, name);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(path)) {
    return reportedMetadata("", directory);
  }
  return reportedMetadata(path, directory);
}

void SmbServer::Impl::rememberMetadata(const char* path, bool directory,
                                       const VfsMetadata& metadata,
                                       uint8_t appliedAttributes) {
  CachedMetadata* selected = nullptr;
  for (CachedMetadata& cached : metadataCache) {
    if (cached.used && asciiEqualNoCase(cached.path, path)) {
      selected = &cached;
      break;
    }
    if (!cached.used && selected == nullptr) {
      selected = &cached;
    } else if (selected != nullptr && selected->used &&
               cached.lastUse < selected->lastUse) {
      selected = &cached;
    }
  }
  if (selected == nullptr) {
    return;
  }
  if (!selected->used || !asciiEqualNoCase(selected->path, path)) {
    selected->value = reportedMetadata(path, directory);
    snprintf(selected->path, sizeof(selected->path), "%s", path);
    selected->used = true;
  }
  ++metadataUseCounter;
  if (metadataUseCounter == 0) {
    metadataUseCounter = 1;
  }
  selected->lastUse = metadataUseCounter;

  if (metadata.attrMask != 0) {
    uint32_t attributes = appliedAttributes & 0x27U;
    if (directory) {
      attributes |= SMB2_FILE_ATTRIBUTE_DIRECTORY;
    } else if (attributes == 0) {
      attributes = SMB2_FILE_ATTRIBUTE_NORMAL;
    }
    selected->value.attributes = attributes;
  }
  if ((metadata.timeMask & 0x01U) != 0 && metadata.createFileTime != 0) {
    selected->value.creationTime = metadata.createFileTime;
  }
  if ((metadata.timeMask & 0x02U) != 0 && metadata.accessFileTime != 0) {
    selected->value.lastAccessTime = metadata.accessFileTime;
  }
  if ((metadata.timeMask & 0x04U) != 0 && metadata.writeFileTime != 0) {
    selected->value.lastWriteTime = metadata.writeFileTime;
    // FAT не хранит отдельное ChangeTime. После записи метаданных оно
    // совпадает с последним изменением содержимого файла.
    selected->value.changeTime = metadata.writeFileTime;
  }
}

void SmbServer::Impl::forgetMetadata(const char* path) {
  for (CachedMetadata& cached : metadataCache) {
    if (cached.used && asciiPathBelongsTo(cached.path, path)) {
      cached = {};
    }
  }
}

void SmbServer::Impl::renameMetadata(const char* oldPath,
                                     const char* newPath) {
  forgetMetadata(newPath);
  const size_t oldLength = strlen(oldPath);
  for (CachedMetadata& cached : metadataCache) {
    if (!cached.used || !asciiPathBelongsTo(cached.path, oldPath)) {
      continue;
    }
    const char* suffix = cached.path + oldLength;
    char updated[kMaxPath + 1] = {};
    const int length = snprintf(updated, sizeof(updated), "%s%s", newPath,
                                suffix);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(updated)) {
      cached = {};
      continue;
    }
    snprintf(cached.path, sizeof(cached.path), "%s", updated);
  }
}

void SmbServer::Impl::fillCreateContextReply(
    const RequestedCreateContexts& requested, uint64_t diskFileId,
    uint64_t volumeId, uint64_t changeTime, uint32_t maximalAccess,
    smb2_create_reply& reply) {
  memset(createContextReply, 0, sizeof(createContextReply));
  size_t used = 0;
  size_t previous = 0;
  bool havePrevious = false;

  if (requested.maximalAccess) {
    uint8_t data[8] = {};
    if (requested.maximalAccessHasTimestamp &&
        requested.maximalAccessTimestamp == changeTime) {
      writeLe32(data, SMB2_STATUS_NONE_MAPPED);
    } else {
      writeLe32(data, SMB2_STATUS_SUCCESS);
      writeLe32(data + 4, maximalAccess);
    }
    const size_t length = appendCreateResponseContext(
        createContextReply, sizeof(createContextReply), used, "MxAc", data,
        sizeof(data));
    if (length != 0) {
      previous = used;
      havePrevious = true;
      used += length;
    }
  }

  // [MS-SMB2] 3.3.5.9.9 запрещает QFid при durable reconnect. Сам сервер
  // durable handle не выдаёт, но входной список контекстов всё равно обязан
  // распознать. DiskFileId берётся из той же функции, что и
  // FileInternalInformation, а VolumeId — из серийного номера FAT. Поэтому
  // два ответа SMB остаются согласованными даже при ограничениях FILEX.
  if (requested.queryOnDiskId && !requested.durableReconnect) {
    uint8_t data[32] = {};
    writeLe64Local(data, diskFileId);
    writeLe64Local(data + 8, volumeId);
    const size_t length = appendCreateResponseContext(
        createContextReply, sizeof(createContextReply), used, "QFid", data,
        sizeof(data));
    if (length != 0) {
      if (havePrevious) {
        writeLe32(createContextReply + previous,
                  static_cast<uint32_t>(used - previous));
      }
      previous = used;
      havePrevious = true;
      used += length;
    }
  }

  reply.create_context = used == 0 ? nullptr : createContextReply;
  reply.create_context_length = static_cast<uint32_t>(used);
}

uint64_t SmbServer::Impl::currentFileTime() const {
  // Фиксировано, чтобы Notepad++ не зацикливал перезагрузку файла.
  const time_t now = 1767225600;  // 2026-01-01 00:00:00 UTC.
  constexpr uint64_t kUnixEpochSeconds = 11644473600ULL;
  constexpr uint64_t kTicksPerSecond = 10000000ULL;
  return (static_cast<uint64_t>(now) + kUnixEpochSeconds) * kTicksPerSecond;
}

smb2_timeval SmbServer::Impl::currentSmb2Time() const {
  const time_t now = 1767225600;  // 2026-01-01 00:00:00 UTC.
  smb2_timeval tv = {};
  tv.tv_sec = now;
  tv.tv_usec = 0;
  return tv;
}

void SmbServer::Impl::fillDirectoryInfo(
    smb2_fileidbothdirectoryinformation& info, uint32_t index,
    bool directory, uint32_t size, const char* parentPath,
    const char* name) const {
  memset(&info, 0, sizeof(info));
  const ReportedMetadata metadata =
      reportedChildMetadata(parentPath, name, directory);
  smb2_win_to_timeval(metadata.creationTime, &info.creation_time);
  smb2_win_to_timeval(metadata.lastAccessTime, &info.last_access_time);
  smb2_win_to_timeval(metadata.lastWriteTime, &info.last_write_time);
  smb2_win_to_timeval(metadata.changeTime, &info.change_time);
  info.file_index = index;
  info.end_of_file = size;
  info.allocation_size = reportedAllocationSize(size, directory);
  info.file_attributes = metadata.attributes;
  // FILE_ID_*_DIRECTORY_INFORMATION и FILE_INTERNAL_INFORMATION обязаны
  // возвращать один идентификатор для одного объекта. Explorer сначала
  // запоминает FileId из каталога, затем сверяет его после CREATE. Хеш только
  // имени без пути здесь, при хеше полного handle->path в QUERY_INFO, заставлял
  // оболочку считать открытый файл другим объектом и не начинать READ.
  info.file_id = directoryChildFileId(parentPath, name);
  info.name = name;
}

bool SmbServer::Impl::fetchReadWindow(Handle& handle) {
  if (activeVfsOffset >= handle.physicalSize) {
    return false;
  }
  const uint32_t remaining = handle.physicalSize - activeVfsOffset;
  // Предел окна задаёт режим открытия: позиционный тракт FILEX держит первые
  // 32 байта страницы под блок параметров, последовательному доступен весь
  // буфер. Просить больше — получить отказ Z80 вместо данных.
  const size_t limit = activeRandomRead ? VfsClient::kFilexTransferWindowSize
                                        : VfsClient::kTransferWindowSize;
  const uint32_t wanted = static_cast<uint32_t>(minimum(
      static_cast<size_t>(remaining), limit));
  VfsResult result;
  if (!requestVfsAt(VfsOperation::kReadAt, activeVfsOffset, wanted, result,
                    kNormalVfsTimeoutMs) ||
      result.transferred == 0) {
    return false;
  }
  activeVfsOffset += result.transferred;
  return true;
}

bool SmbServer::Impl::activateRead(int slot, uint32_t offset) {
  Handle& handle = handles[slot];
  if (activeSlot == slot && activeMode == ActiveMode::kRead &&
      activeRandomRead && activeLogicalOffset == offset) {
    return true;
  }
  if (!closeActive(true) || !resetBuffers()) {
    return false;
  }
  VfsResult result;
  bool random = true;
  if (!requestVfs(VfsOperation::kOpenRandom, handle.path, 0, result,
                  kNormalVfsTimeoutMs)) {
    if (offset == 0 &&
        requestVfs(VfsOperation::kOpenRead, handle.path, 0, result,
                   kNormalVfsTimeoutMs)) {
      random = false;
    } else {
      return false;
    }
  }
  activeSlot = slot;
  activeMode = ActiveMode::kRead;
  activeRandomWrite = false;
  activeRandomRead = random;
  activeLogicalOffset = offset;
  activeVfsOffset = offset;
  return true;
}

bool SmbServer::Impl::activateWrite(int slot, uint32_t offset) {
  // Правка тома делает копию файла в PSRAM недостоверной: отдать по сети
  // устаревшие байты хуже, чем прочитать файл заново.
  dropFileCache();
  Handle& handle = handles[slot];
  if (activeSlot == slot && activeMode == ActiveMode::kWrite &&
      activeLogicalOffset == offset && activeRandomWrite) {
    return true;
  }
  if (!closeActive(true) || !resetBuffers()) {
    return false;
  }
  VfsResult result;
  // Последовательный FILEX mode=1 подтверждает неполное последнее окно до
  // фактического APPEND и переносит его ошибку на CLOSE. В реальной SD->SD
  // серии 0.6.80 WRITE 44032 и SET_BASIC_INFORMATION были успешны, но этот
  // финальный APPEND отказал, CLOSE вернул STATUS_IO_DEVICE_ERROR и новый файл
  // пришлось удалить. SMB использует только mode=3: каждое WRITE_AT-окно
  // физически завершено до ответа WRITE, а SET_METADATA работает в том же
  // открытом контексте. Mode=1 остаётся внутри createEmptyFile только для
  // материализации пустого уже закрытого файла, где отложенных данных нет.
  if (!requestVfs(VfsOperation::kOpenRandom, handle.path, 0, result,
                  kMutateVfsTimeoutMs)) {
    return false;
  }
  activeSlot = slot;
  activeMode = ActiveMode::kWrite;
  activeRandomWrite = true;
  activeLogicalOffset = offset;
  activeVfsOffset = offset;
  return true;
}

bool SmbServer::Impl::commitReservedSize(int slot) {
  if (slot < 0 || static_cast<size_t>(slot) >= kHandleCount) {
    return false;
  }
  Handle& handle = handles[slot];
  if (handle.deletePending || !handle.sizeReserved ||
      handle.reservedSize <= handle.physicalSize) {
    handle.sizeReserved = false;
    handle.ownsSizeReservation = false;
    return true;
  }
  // Наследованный резерв нужен этому Open только для согласованного EOF в
  // QUERY_INFO. Закрытие/чтение read-only наблюдателя не является операцией
  // записи и не должно ждать мост либо менять FAT.
  if (!handle.ownsSizeReservation) {
    return true;
  }

  // Правка тома делает копию файла в PSRAM недостоверной: отдать по сети
  // устаревшие байты хуже, чем прочитать файл заново.
  dropFileCache();

  const uint32_t target = handle.reservedSize;
  if (!activateWrite(slot, handle.physicalSize)) {
    return false;
  }
  VfsResult result;
  if (!requestVfs(VfsOperation::kSetEof, nullptr, target, result,
                  kMutateVfsTimeoutMs) || result.size != target) {
    closeActive(false);
    return false;
  }

  invalidateFsInfo();
  updateSharedPhysicalSize(handle.path, target);
  handle.reservedSize = target;
  handle.sizeReserved = false;
  handle.ownsSizeReservation = false;
  handle.metadataDirty = true;
  activeLogicalOffset = target;
  activeVfsOffset = target;
  sendOperation("SETEOF", handle.path);
  return closeActive(true);
}

void SmbServer::Impl::deferMetadata(Handle& handle,
                                    const VfsMetadata& metadata) {
  VfsMetadata& pending = handle.pendingMetadata;
  if (metadata.attrMask != 0) {
    pending.attrValue = static_cast<uint8_t>(
        (pending.attrValue & ~metadata.attrMask) |
        (metadata.attrValue & metadata.attrMask));
    pending.attrMask |= metadata.attrMask;
  }
  if ((metadata.timeMask & 0x01) != 0) {
    pending.createTenth = metadata.createTenth;
    pending.createTime = metadata.createTime;
    pending.createDate = metadata.createDate;
    pending.createFileTime = metadata.createFileTime;
  }
  if ((metadata.timeMask & 0x02) != 0) {
    pending.accessDate = metadata.accessDate;
    pending.accessFileTime = metadata.accessFileTime;
  }
  if ((metadata.timeMask & 0x04) != 0) {
    pending.writeTime = metadata.writeTime;
    pending.writeDate = metadata.writeDate;
    pending.writeFileTime = metadata.writeFileTime;
  }
  pending.timeMask |= metadata.timeMask;
  handle.metadataPending = true;
}

bool SmbServer::Impl::applyPendingMetadata(int slot) {
  if (slot < 0 || static_cast<size_t>(slot) >= kHandleCount) {
    return false;
  }
  Handle& handle = handles[slot];
  if (!handle.metadataPending) {
    return true;
  }
  VfsResult result = {};
  bool applied = false;
  if (activeSlot == slot && activeMode == ActiveMode::kWrite &&
      activeRandomWrite) {
    applied = requestMetadata(handle.pendingMetadata, result);
  } else if (activateWrite(slot, handle.position)) {
    applied = requestMetadata(handle.pendingMetadata, result);
  }
  if (!applied) {
    return false;
  }
  rememberMetadata(handle.path, handle.directory, handle.pendingMetadata,
                   result.appliedAttributes);
  handle.metadataPending = false;
  handle.pendingMetadata = {};
  handle.metadataDirty = true;
  sendOperation("ATTR", handle.path);
  return true;
}

bool SmbServer::Impl::activateDirectory(int slot) {
  Handle& handle = handles[slot];
  if (activeSlot == slot && activeMode == ActiveMode::kDirectory) {
    return true;
  }
  if (!closeActive(true)) {
    return false;
  }
  VfsResult result;
  if (!requestVfs(VfsOperation::kOpenDirectory, handle.path, 0, result,
                  kNormalVfsTimeoutMs)) {
    return false;
  }
  activeSlot = slot;
  activeMode = ActiveMode::kDirectory;
  activeRandomWrite = false;
  activeLogicalOffset = 0;
  activeVfsOffset = 0;

  // Первый физический проход одновременно является источником будущего
  // снимка. Никакого второго чтения каталога здесь не запускается.
  beginDirectoryCacheBuild(slot, handle);

  // Любая другая VFS-операция меняет рабочий поток WC. При возврате к каталогу
  // открываем его заново и молча пропускаем уже выданные записи.
  for (uint32_t index = 0; index < handle.directoryIndex; ++index) {
    if (!requestVfs(VfsOperation::kReadDirectory, nullptr, 0, result,
                    kNormalVfsTimeoutMs) ||
        result.atEnd) {
      if (directoryCacheBuildSlot == slot &&
          directoryCacheBuildGeneration == handle.generation) {
        abortDirectoryCacheBuild();
      }
      handle.directoryEnded = true;
      return result.atEnd;
    }
  }
  return true;
}

int SmbServer::Impl::replyStatus(smb2_context* smb2, uint8_t command,
                                 uint32_t status) {
  smb2_error_reply errorReply = {};
  smb2_pdu* pdu = smb2_cmd_error_reply_async(
      smb2, &errorReply, command, static_cast<int>(status), nullptr, nullptr);
  if (pdu == nullptr) {
    return -1;
  }
  smb2_set_pdu_message_id(smb2, pdu,
                          smb2_get_last_request_message_id(smb2));
  smb2_queue_pdu(smb2, pdu);
  return 1;
}

bool SmbServer::Impl::queueAsyncStatus(smb2_context* smb2, uint8_t command,
                                       uint32_t status,
                                       uint64_t messageId) {
  if (smb2 == nullptr || messageId == 0) {
    return false;
  }
  smb2_error_reply errorReply = {};
  smb2_pdu* pdu = smb2_cmd_error_reply_async(
      smb2, &errorReply, command, static_cast<int>(status), nullptr, nullptr);
  if (pdu == nullptr) {
    return false;
  }
  smb2_set_pdu_message_id(smb2, pdu, messageId);
  smb2_queue_pdu(smb2, pdu);
  return true;
}

bool SmbServer::Impl::queueIoInterimPending(smb2_context* context,
                                            uint8_t command,
                                            uint64_t messageId,
                                            bool& pendingSent) {
  if (context == nullptr || messageId == 0 || pendingSent) {
    return context != nullptr && messageId != 0;
  }
  // Сначала сжимаем окно: STATUS_PENDING продлевает тайм-аут Windows, но не
  // должен немедленно вернуть кредит под ещё один медленный запрос.
  if (smb2_set_server_credit_target(context, 1) != 0 ||
      !queueAsyncStatus(context, command, SMB2_STATUS_PENDING, messageId)) {
    return false;
  }
  pendingSent = true;
  diagnosticLogEvent("SMB interim-pending cmd=%u mid=%llu",
                     static_cast<unsigned>(command),
                     static_cast<unsigned long long>(messageId));
  return true;
}

void SmbServer::Impl::restoreServerCreditsIfIoIdle(smb2_context* context) {
  if (context == nullptr) {
    return;
  }
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    if ((asyncReads[index].used && asyncReads[index].context == context) ||
        (asyncWrites[index].used && asyncWrites[index].context == context)) {
      return;
    }
  }
  for (const QueuedRead* pending = queuedReadHead; pending != nullptr;
       pending = pending->next) {
    if (pending->context == context) {
      return;
    }
  }
  (void)smb2_set_server_credit_target(context, SMB2_SERVER_CREDIT_TARGET);
}

void SmbServer::Impl::pollIoInterimPending() {
  const uint32_t now = millis();
  const uint32_t delay = longIoInterimPendingMs();
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    AsyncRead& pending = asyncReads[index];
    if (pending.used && pending.context != nullptr && !pending.pendingSent &&
        static_cast<uint32_t>(now - pending.requestStartedMs) >= delay &&
        !queueIoInterimPending(pending.context, SMB2_READ,
                               pending.messageId, pending.pendingSent)) {
      smb2_close_context(pending.context);
      return;
    }
  }
  for (QueuedRead* pending = queuedReadHead; pending != nullptr;
       pending = pending->next) {
    if (pending->context != nullptr && !pending->pendingSent &&
        static_cast<uint32_t>(now - pending->requestStartedMs) >= delay &&
        !queueIoInterimPending(pending->context, SMB2_READ,
                               pending->messageId, pending->pendingSent)) {
      smb2_close_context(pending->context);
      return;
    }
  }
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    AsyncWrite& pending = asyncWrites[index];
    if (pending.used && pending.context != nullptr && !pending.pendingSent &&
        static_cast<uint32_t>(now - pending.requestStartedMs) >= delay &&
        !queueIoInterimPending(pending.context, SMB2_WRITE,
                               pending.messageId, pending.pendingSent)) {
      smb2_close_context(pending.context);
      return;
    }
  }
}

bool SmbServer::Impl::queueAsyncWriteReply(smb2_context* smb2,
                                           uint64_t messageId,
                                           uint32_t count) {
  if (smb2 == nullptr || messageId == 0) {
    return false;
  }
  smb2_write_reply reply = {};
  reply.count = count;
  reply.remaining = 0;
  smb2_pdu* pdu = smb2_cmd_write_reply_async(smb2, &reply, nullptr, nullptr);
  if (pdu == nullptr) {
    return false;
  }
  smb2_set_pdu_message_id(smb2, pdu, messageId);
  smb2_queue_pdu(smb2, pdu);
  return true;
}

bool SmbServer::Impl::queueAsyncReadReply(smb2_context* smb2,
                                          uint64_t messageId,
                                          const uint8_t* data,
                                          uint32_t count) {
  if (smb2 == nullptr || messageId == 0 || data == nullptr || count == 0) {
    return false;
  }
  uint8_t* owned = static_cast<uint8_t*>(heap_caps_malloc(
      count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (owned == nullptr) {
    return false;
  }
  memcpy(owned, data, count);
  smb2_read_reply reply = {};
  reply.data = owned;
  reply.data_length = count;
  reply.data_remaining = 0;
  // Начиная с вызова encoder буфер принадлежит PDU libsmb2 и освобождается
  // его iovector-ом, в том числе при большинстве ошибок сборки пакета.
  smb2_pdu* pdu = smb2_cmd_read_reply_async(smb2, &reply, nullptr, nullptr);
  if (pdu == nullptr) {
    return false;
  }
  smb2_set_pdu_message_id(smb2, pdu, messageId);
  smb2_queue_pdu(smb2, pdu);
  return true;
}

void SmbServer::Impl::pollAsyncRead() {
  if (activeAsyncRead >= 0) {
    AsyncRead& current = asyncReads[activeAsyncRead];
    Handle* handle = current.slot >= 0 &&
                             static_cast<size_t>(current.slot) < kHandleCount &&
                             handles[current.slot].used &&
                             handles[current.slot].generation ==
                                 current.generation
                         ? &handles[current.slot]
                         : nullptr;

    if (current.inFlight) {
      VfsResult result;
      if (!bridge.takeResult(result)) {
        return;
      }
      current.inFlight = false;
      const uint32_t transferred = static_cast<uint32_t>(minimum(
          static_cast<size_t>(result.transferred),
          static_cast<size_t>(current.windowLength)));
      if (current.cancelRequested || current.context == nullptr) {
        const uint32_t status = current.cancelRequested
                                    ? SMB2_STATUS_CANCELLED
                                    : SMB2_STATUS_IO_DEVICE_ERROR;
        discardAsyncReadData(activeAsyncRead);
        completeAsyncReadWithStatus(activeAsyncRead, status, true);
        return;
      }
      if (!result.success || transferred == 0 ||
          transferred != result.transferred) {
        const uint32_t status = result.status != 0
                                    ? smbStatusFromFilex(result.status)
                                    : SMB2_STATUS_IO_DEVICE_ERROR;
        diagnosticLogEvent(
            "SMB async-read-fail status=%u moved=%lu expected=%lu error=%s",
            static_cast<unsigned>(result.status),
            static_cast<unsigned long>(result.transferred),
            static_cast<unsigned long>(current.windowLength), result.error);
        discardAsyncReadData(activeAsyncRead);
        completeAsyncReadWithStatus(activeAsyncRead, status, true);
        return;
      }
      current.windowLength = 0;
      current.lastProgressMs = millis();
    }

    if (handle == nullptr || current.cancelRequested ||
        current.context == nullptr) {
      const uint32_t status = current.cancelRequested
                                  ? SMB2_STATUS_CANCELLED
                                  : SMB2_STATUS_IO_DEVICE_ERROR;
      discardAsyncReadData(activeAsyncRead);
      completeAsyncReadWithStatus(activeAsyncRead, status, true);
      return;
    }

    const size_t ready = bridge.vfsToNetworkAvailable();
    if (ready != 0 && current.filled < current.length) {
      const size_t part = minimum(
          ready, static_cast<size_t>(current.length - current.filled));
      const size_t got = bridge.readForNetwork(
          asyncIoBuffers[activeAsyncRead] + current.filled, part);
      if (got == 0) {
        completeAsyncReadWithStatus(activeAsyncRead,
                                    SMB2_STATUS_IO_DEVICE_ERROR, true);
        return;
      }
      const uint32_t progressOffset = current.offset + current.filled;
      current.filled += static_cast<uint32_t>(got);
      activeLogicalOffset += static_cast<uint32_t>(got);
      current.lastProgressMs = millis();
      // SMB-ответ остаётся целым 64-КиБ запросом, но экрану не нужно ждать
      // завершения всех четырёх физических окон FILEX.
      if (!noteTransferProgress(*handle, TransferProgressMode::kRead,
                                progressOffset, static_cast<uint32_t>(got))) {
        diagnosticLogEvent("SMB progress-range-oom mode=READ path=%s",
                           handle->path);
      }
      sendProgress(*handle, "READ", handle->progressBytes, false);
    }

    if (current.filled == current.length) {
      smb2_context* completedContext = current.context;
      const uint64_t completedMessageId = current.messageId;
      const uint32_t completedLength = current.length;
      const int completedIndex = activeAsyncRead;
      handle->position = current.offset + current.filled;
      activeAsyncRead = -1;
      appendFileCache(current.offset, asyncIoBuffers[completedIndex],
                      completedLength);
      if (!queueAsyncReadReply(completedContext, completedMessageId,
                               asyncIoBuffers[completedIndex],
                               completedLength)) {
        current = {};
        if (asyncReadCount != 0) {
          --asyncReadCount;
        }
        restoreServerCreditsIfIoIdle(completedContext);
        smb2_close_context(completedContext);
        return;
      }
      const uint32_t left = handle->physicalSize > handle->position
                                ? handle->physicalSize - handle->position
                                : 0;
      diagnosticLogEvent("SMB read-ok bytes=%lu left=%lu path=%s",
                         static_cast<unsigned long>(completedLength),
                         static_cast<unsigned long>(left),
                         handle->path);
      sendOperation("READ", handle->path);
      current = {};
      if (asyncReadCount != 0) {
        --asyncReadCount;
      }
      restoreServerCreditsIfIoIdle(completedContext);
      return;
    }

    if (bridge.requestPending()) {
      return;
    }
    const uint32_t physicalOffset = current.offset + current.filled;
    if (physicalOffset >= handle->physicalSize) {
      current.length = current.filled;
      return;
    }
    const size_t physicalLimit =
        activeRandomRead ? VfsClient::kFilexTransferWindowSize
                         : VfsClient::kTransferWindowSize;
    const size_t wanted = minimum(
        minimum(static_cast<size_t>(current.length - current.filled),
                static_cast<size_t>(handle->physicalSize - physicalOffset)),
        minimum(physicalLimit, bridge.vfsToNetworkFree()));
    if (wanted == 0 ||
        !bridge.submitAt(VfsOperation::kReadAt, physicalOffset,
                         static_cast<uint32_t>(wanted))) {
      completeAsyncReadWithStatus(activeAsyncRead,
                                  SMB2_STATUS_IO_DEVICE_ERROR, true);
      return;
    }
    current.windowLength = static_cast<uint32_t>(wanted);
    current.inFlight = true;
    return;
  }

  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    AsyncRead& pending = asyncReads[index];
    if (!pending.used) {
      continue;
    }
    const bool validHandle =
        pending.slot >= 0 &&
        static_cast<size_t>(pending.slot) < kHandleCount &&
        handles[pending.slot].used &&
        handles[pending.slot].generation == pending.generation;
    if (pending.cancelRequested || !validHandle) {
      completeAsyncReadWithStatus(
          static_cast<int>(index),
          pending.cancelRequested ? SMB2_STATUS_CANCELLED
                                  : SMB2_STATUS_IO_DEVICE_ERROR,
          false);
      return;
    }
  }

  const int nextIndex = findReadyAsyncRead();
  if (nextIndex < 0) {
    return;
  }
  AsyncRead& next = asyncReads[nextIndex];
  const uint64_t writeSequence = oldestAsyncWriteSequence();
  const uint64_t directorySequence = oldestAsyncDirectorySequence();
  const uint64_t createSequence = oldestAsyncCreateSequence();
  const uint64_t closeSequence = oldestAsyncCloseSequence();
  if (writeSequence < next.sequence || directorySequence < next.sequence ||
      createSequence < next.sequence || closeSequence < next.sequence) {
    return;
  }
  if (bridge.requestPending()) {
    return;
  }
  if (!activateRead(next.slot, next.offset)) {
    completeAsyncReadWithStatus(nextIndex, SMB2_STATUS_IO_DEVICE_ERROR, false);
    return;
  }
  activeAsyncRead = nextIndex;
  next.lastProgressMs = millis();
  // activateRead может оставить в выходном кольце хвост уже прочитанного
  // физического окна. Повторный вход сначала заберёт его, а затем отправит
  // core 1 следующий запрос.
  pollAsyncRead();
}

void SmbServer::Impl::pollAsyncWrite() {
  if (activeAsyncWrite >= 0) {
    VfsResult result = {};
    if (!bridge.takeResult(result)) {
      return;
    }

    const int completedIndex = activeAsyncWrite;
    activeAsyncWrite = -1;
    AsyncWrite& completed = asyncWrites[completedIndex];
    Handle* handle = completed.slot >= 0 &&
                             static_cast<size_t>(completed.slot) <
                                 kHandleCount &&
                             handles[completed.slot].used &&
                             handles[completed.slot].generation ==
                                 completed.generation
                         ? &handles[completed.slot]
                         : nullptr;
    const uint32_t transferred = static_cast<uint32_t>(minimum(
        static_cast<size_t>(result.transferred),
        static_cast<size_t>(completed.windowLength)));
    if (transferred != 0) {
      completed.lastProgressMs = millis();
    }
    if (handle != nullptr && transferred != 0) {
      const uint32_t end = completed.offset + completed.flushed + transferred;
      if (end > handle->physicalSize) {
        // Локальная арифметика и правка снимка — без обмена с мостом.
        updateSharedPhysicalSize(handle->path, end);
      }
      handle->metadataDirty = true;
      handle->position = end;
      activeLogicalOffset = end;
      activeVfsOffset = end;
      if (handle->sizeReserved &&
          handle->physicalSize >= handle->reservedSize) {
        handle->sizeReserved = false;
        handle->ownsSizeReservation = false;
      }
    }

    const bool succeeded = result.success && handle != nullptr &&
                           completed.windowLength != 0 &&
                           transferred == completed.windowLength &&
                           !completed.cancelRequested &&
                           (completed.replied || completed.context != nullptr);
    if (!succeeded) {
      if (completed.cancelRequested) {
        completeCancelledAsyncWrite(completedIndex, true);
        return;
      }
      const bool detached = completed.context == nullptr;
      const uint32_t status =
          result.status != 0 ? smbStatusFromFilex(result.status)
                             : SMB2_STATUS_IO_DEVICE_ERROR;
      failAsyncWrites(status);
      if (detached) {
        releaseClientHandles(nullptr);
        diagnosticLogEvent("SMB cleanup-deferred done");
      }
      return;
    }

    const uint32_t progressOffset = completed.offset + completed.flushed;
    completed.flushed += transferred;
    completed.inFlight = false;
    completed.windowLength = 0;
    if (!noteTransferProgress(*handle, TransferProgressMode::kWrite,
                              progressOffset, transferred)) {
      diagnosticLogEvent("SMB progress-range-oom mode=WRITE path=%s",
                         handle->path);
    }
    sendProgress(*handle, "WRITE", handle->progressBytes, false);

    const bool finished = completed.flushed == completed.length;
    // Любой WRITE получает ответ только после полного физического завершения.
    // Поэтому незавершённый запрос остаётся адресуемым для SMB CANCEL.
    if (finished && !completed.replied) {
      smb2_context* completedContext = completed.context;
      if (completedContext == nullptr ||
          !queueAsyncWriteReply(completedContext, completed.messageId,
                                completed.length)) {
        handle->failed = true;
        completed = {};
        if (asyncWriteCount != 0) {
          --asyncWriteCount;
        }
        restoreServerCreditsIfIoIdle(completedContext);
        failAsyncWrites(SMB2_STATUS_IO_DEVICE_ERROR);
        if (completedContext != nullptr) {
          smb2_close_context(completedContext);
        }
        return;
      }
      completed.replied = true;
    }

    if (finished) {
      const bool detached = completed.context == nullptr;
      smb2_context* const completedContext = completed.context;
      smb2_context* const owner = completed.owner;
      sendOperation("WRITE", handle->path);
      notifyChange(handle->path, SMB2_NOTIFY_CHANGE_FILE_ACTION_MODIFIED,
                   SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_SIZE |
                       SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_LAST_WRITE);
      completed = {};
      if (asyncWriteCount != 0) {
        --asyncWriteCount;
      }
      restoreServerCreditsIfIoIdle(completedContext);
      if (detached) {
        closeActive(true);
        releaseDetachedOwnerIfIdle(owner);
      }
    }
  }

  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    if (asyncWrites[index].used && asyncWrites[index].cancelRequested) {
      completeCancelledAsyncWrite(static_cast<int>(index), false);
      return;
    }
  }

  const int nextIndex = findReadyAsyncWrite();
  if (nextIndex < 0) {
    return;
  }
  AsyncWrite& next = asyncWrites[nextIndex];
  const uint64_t readSequence = oldestAsyncReadSequence();
  const uint64_t directorySequence = oldestAsyncDirectorySequence();
  const uint64_t createSequence = oldestAsyncCreateSequence();
  const uint64_t closeSequence = oldestAsyncCloseSequence();
  if (readSequence < next.sequence || directorySequence < next.sequence ||
      createSequence < next.sequence || closeSequence < next.sequence) {
    return;
  }
  const uint32_t remaining = next.length - next.flushed;
  const uint32_t window = static_cast<uint32_t>(minimum(
      bridge.ringCapacity(), VfsClient::kTransferWindowSize));
  const uint32_t part = static_cast<uint32_t>(minimum(
      static_cast<size_t>(remaining), static_cast<size_t>(window)));
  const uint32_t nextOffset = next.offset + next.flushed;
  if (bridge.requestPending() || bridge.networkToVfsAvailable() != 0 ||
      part == 0 || bridge.networkToVfsFree() < part) {
    return;
  }
  if (!activateWrite(next.slot, nextOffset) ||
      bridge.writeFromNetwork(asyncIoBuffers[nextIndex] + next.flushed,
                              part) != part ||
      !bridge.submitAt(VfsOperation::kWriteAt, nextOffset, part)) {
    resetBuffers();
    failAsyncWrites(SMB2_STATUS_IO_DEVICE_ERROR);
    return;
  }
  next.inFlight = true;
  next.windowLength = part;
  next.lastProgressMs = millis();
  activeAsyncWrite = nextIndex;
}

int SmbServer::Impl::allocateAsyncDirectory() const {
  if (asyncDirectoryCount >= kAsyncDirectoryQueueDepth) {
    return -1;
  }
  for (size_t index = 0; index < kAsyncDirectoryQueueDepth; ++index) {
    if (!asyncDirectories[index].used) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

bool SmbServer::Impl::enqueueAsyncDirectory(
    smb2_context* context, uint64_t messageId, int slot, uint32_t generation,
    const smb2_query_directory_request& request) {
  const int index = allocateAsyncDirectory();
  if (index < 0 || context == nullptr || messageId == 0 || slot < 0) {
    return false;
  }
  AsyncDirectory& pending = asyncDirectories[index];
  pending = {};
  pending.used = true;
  pending.context = context;
  pending.messageId = messageId;
  pending.sequence = nextAsyncVfsSequence();
  pending.slot = slot;
  pending.generation = generation;
  pending.outputBufferLength = request.output_buffer_length;
  pending.informationClass = request.file_information_class;
  pending.flags = request.flags;
  pending.hasName = request.name != nullptr && request.name[0] != 0;
  if (pending.hasName) {
    snprintf(pending.name, sizeof(pending.name), "%s", request.name);
  }
  pending.lastProgressMs = millis();
  ++asyncDirectoryCount;
  diagnosticLogEvent("SMB directory-deferred mid=%llu waiting=%u",
                     static_cast<unsigned long long>(messageId),
                     static_cast<unsigned>(asyncDirectoryCount));
  return true;
}

bool SmbServer::Impl::queueAsyncDirectoryReply(
    const AsyncDirectory& pending, Handle& handle, const char* name,
    bool directory, uint32_t size, uint32_t fileIndex) {
  if (pending.context == nullptr || name == nullptr || name[0] == 0) {
    return false;
  }
  snprintf(directoryName, sizeof(directoryName), "%s", name);
  memset(&directoryInfo, 0, sizeof(directoryInfo));
  fillDirectoryInfo(directoryInfo, fileIndex, directory, size, handle.path,
                    directoryName);

  smb2_query_directory_request request = {};
  request.file_information_class = pending.informationClass;
  request.flags = pending.flags;
  request.output_buffer_length = pending.outputBufferLength;
  smb2_query_directory_reply reply = {};
  reply.output_buffer = reinterpret_cast<uint8_t*>(&directoryInfo);
  reply.output_buffer_length = static_cast<uint32_t>(
      padTo8(sizeof(smb2_fileidbothdirectoryinformation)));
  smb2_pdu* pdu = smb2_cmd_query_directory_reply_async(
      pending.context, &request, &reply, nullptr, nullptr);
  if (pdu == nullptr) {
    return false;
  }
  // Запоминаем запись до постановки ответа в TCP. Следующий запрос Проводника
  // может прийти сразу же и пересечься с уже запущенным FINDNEXT.
  handle.directoryLastValid = true;
  handle.directoryLastIsDirectory = directory;
  handle.directoryLastSize = size;
  snprintf(handle.directoryLastName, sizeof(handle.directoryLastName), "%s",
           name);
  smb2_set_pdu_message_id(pending.context, pdu, pending.messageId);
  smb2_queue_pdu(pending.context, pdu);
  sendOperation("DIR", handle.path);
  return true;
}

void SmbServer::Impl::completeAsyncDirectory(int index, uint32_t status) {
  if (index < 0 || static_cast<size_t>(index) >= kAsyncDirectoryQueueDepth ||
      !asyncDirectories[index].used) {
    return;
  }
  AsyncDirectory& pending = asyncDirectories[index];
  smb2_context* context = pending.context;
  const bool queued = context != nullptr &&
                      queueAsyncStatus(context, SMB2_QUERY_DIRECTORY, status,
                                       pending.messageId);
  pending = {};
  if (asyncDirectoryCount != 0) {
    --asyncDirectoryCount;
  }
  if (activeAsyncDirectory == index) {
    activeAsyncDirectory = -1;
  }
  if (context != nullptr && !queued) {
    smb2_close_context(context);
  }
}

void SmbServer::Impl::failAsyncDirectories(uint32_t status) {
  for (size_t index = 0; index < kAsyncDirectoryQueueDepth; ++index) {
    AsyncDirectory& pending = asyncDirectories[index];
    if (!pending.used) {
      continue;
    }
    smb2_context* context = pending.context;
    const bool queued = context == nullptr ||
                        queueAsyncStatus(context, SMB2_QUERY_DIRECTORY, status,
                                         pending.messageId);
    if (static_cast<int>(index) == activeAsyncDirectory &&
        pending.inFlight) {
      // Exchange всё ещё принадлежит core 1. Ответ Windows уже завершён, но
      // слот живёт до takeResult(), чтобы не перезаписать общую память моста.
      pending.context = nullptr;
      pending.replied = true;
    } else {
      pending = {};
      if (asyncDirectoryCount != 0) {
        --asyncDirectoryCount;
      }
      if (static_cast<int>(index) == activeAsyncDirectory) {
        activeAsyncDirectory = -1;
      }
    }
    if (context != nullptr && !queued) {
      smb2_close_context(context);
    }
  }
}

void SmbServer::Impl::dropAsyncDirectoriesForOwner(smb2_context* owner) {
  if (owner == nullptr) {
    return;
  }
  for (size_t index = 0; index < kAsyncDirectoryQueueDepth; ++index) {
    AsyncDirectory& pending = asyncDirectories[index];
    if (!pending.used || pending.context != owner) {
      continue;
    }
    if (static_cast<int>(index) == activeAsyncDirectory &&
        pending.inFlight) {
      pending.context = nullptr;
      pending.replied = true;
      continue;
    }
    pending = {};
    if (asyncDirectoryCount != 0) {
      --asyncDirectoryCount;
    }
    if (static_cast<int>(index) == activeAsyncDirectory) {
      activeAsyncDirectory = -1;
    }
  }
}

bool SmbServer::Impl::cancelAsyncDirectory(smb2_context* owner,
                                           uint64_t messageId) {
  for (AsyncDirectory& pending : asyncDirectories) {
    if (pending.used && pending.context == owner &&
        pending.messageId == messageId) {
      pending.cancelRequested = true;
      return true;
    }
  }
  return false;
}

int SmbServer::Impl::allocateAsyncCreate() const {
  if (asyncCreateCount >= kAsyncCreateQueueDepth) {
    return -1;
  }
  for (size_t index = 0; index < kAsyncCreateQueueDepth; ++index) {
    if (!asyncCreates[index].used) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

bool SmbServer::Impl::enqueueAsyncCreate(
    smb2_context* context, uint64_t messageId, uint32_t treeId,
    uint32_t desiredAccess, uint32_t createOptions,
    const RequestedCreateContexts& requestedContexts, uint64_t volumeId,
    bool statRequired, const char* path) {
  const int index = allocateAsyncCreate();
  if (index < 0 || context == nullptr || messageId == 0 || treeId == 0 ||
      path == nullptr || path[0] == 0) {
    return false;
  }
  AsyncCreate& pending = asyncCreates[index];
  pending = {};
  pending.used = true;
  pending.context = context;
  pending.owner = context;
  pending.messageId = messageId;
  pending.sequence = nextAsyncVfsSequence();
  pending.volumeId = volumeId;
  pending.treeId = treeId;
  pending.desiredAccess = desiredAccess;
  pending.createOptions = createOptions;
  pending.requestedContexts = requestedContexts;
  pending.phase = statRequired ? AsyncCreatePhase::kStat
                               : AsyncCreatePhase::kPrepare;
  pending.lastProgressMs = millis();
  snprintf(pending.path, sizeof(pending.path), "%s", path);
  ++asyncCreateCount;
  diagnosticLogEvent("SMB mkdir-deferred mid=%llu waiting=%u path=%s",
                     static_cast<unsigned long long>(messageId),
                     static_cast<unsigned>(asyncCreateCount), path);
  return true;
}

bool SmbServer::Impl::queueAsyncCreateReply(AsyncCreate& pending) {
  if (pending.context == nullptr || pending.messageId == 0) {
    return false;
  }
  const Tree* tree = findTree(pending.treeId);
  if (tree == nullptr || tree->owner != pending.context) {
    return false;
  }
  const int slot = allocateHandle(pending.context, pending.treeId);
  if (slot < 0) {
    return queueAsyncStatus(pending.context, SMB2_CREATE,
                            SMB2_STATUS_INSUFFICIENT_RESOURCES,
                            pending.messageId);
  }

  Handle& handle = handles[slot];
  handle.directory = true;
  handle.writable =
      (pending.desiredAccess &
       (SMB2_FILE_WRITE_DATA | SMB2_FILE_APPEND_DATA | SMB2_GENERIC_WRITE |
        SMB2_DELETE | SMB2_FILE_WRITE_ATTRIBUTES | SMB2_FILE_WRITE_EA)) != 0;
  handle.deletePending =
      (pending.createOptions & SMB2_FILE_DELETE_ON_CLOSE) != 0;
  handle.createdNew = true;
  handle.physicalSize = 0;
  handle.openedSize = 0;
  handle.reservedSize = 0;
  snprintf(handle.path, sizeof(handle.path), "%s", pending.path);

  smb2_create_reply reply = {};
  memcpy(reply.file_id, handle.fileId, SMB2_FD_SIZE);
  const ReportedMetadata metadata = reportedMetadata(handle.path, true);
  reply.creation_time = metadata.creationTime;
  reply.last_access_time = metadata.lastAccessTime;
  reply.last_write_time = metadata.lastWriteTime;
  reply.change_time = metadata.changeTime;
  reply.oplock_level = SMB2_OPLOCK_LEVEL_NONE;
  reply.create_action = kCreateCreated;
  reply.allocation_size = 0;
  reply.end_of_file = 0;
  reply.file_attributes = metadata.attributes;
  fillCreateContextReply(pending.requestedContexts,
                         directoryFileId(handle.path), pending.volumeId,
                         reply.change_time, 0x001F01FFUL, reply);

  smb2_pdu* pdu =
      smb2_cmd_create_reply_async(pending.context, &reply, nullptr, nullptr);
  if (pdu == nullptr) {
    releaseHandle(slot);
    return false;
  }
  smb2_set_pdu_message_id(pending.context, pdu, pending.messageId);
  smb2_queue_pdu(pending.context, pdu);
  diagnosticLogEvent("SMB mkdir-ok slot=%d path=%s", slot, pending.path);
  sendOperation("OPEN", pending.path);
  notifyChange(pending.path, SMB2_NOTIFY_CHANGE_FILE_ACTION_ADDED,
               SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_DIR_NAME);
  return true;
}

void SmbServer::Impl::completeAsyncCreate(int index, uint32_t status) {
  if (index < 0 || static_cast<size_t>(index) >= kAsyncCreateQueueDepth ||
      !asyncCreates[index].used) {
    return;
  }
  AsyncCreate& pending = asyncCreates[index];
  smb2_context* const context = pending.context;
  smb2_context* const owner = pending.owner;
  const uint64_t messageId = pending.messageId;
  const bool detached = context == nullptr;
  pending = {};
  if (asyncCreateCount != 0) {
    --asyncCreateCount;
  }
  if (activeAsyncCreate == index) {
    activeAsyncCreate = -1;
  }
  const bool queued = context == nullptr ||
                      queueAsyncStatus(context, SMB2_CREATE, status, messageId);
  if (!queued && context != nullptr) {
    smb2_close_context(context);
  }
  if (detached) {
    releaseDetachedOwnerIfIdle(owner);
  }
}

void SmbServer::Impl::failAsyncCreates(uint32_t status) {
  for (size_t index = 0; index < kAsyncCreateQueueDepth; ++index) {
    AsyncCreate& pending = asyncCreates[index];
    if (!pending.used) {
      continue;
    }
    smb2_context* const context = pending.context;
    if (context != nullptr &&
        !queueAsyncStatus(context, SMB2_CREATE, status, pending.messageId)) {
      smb2_close_context(context);
    }
    if (static_cast<int>(index) == activeAsyncCreate && pending.inFlight) {
      pending.context = nullptr;
      pending.cancelRequested = true;
      continue;
    }
    pending = {};
    if (asyncCreateCount != 0) {
      --asyncCreateCount;
    }
    if (activeAsyncCreate == static_cast<int>(index)) {
      activeAsyncCreate = -1;
    }
  }
}

void SmbServer::Impl::dropAsyncCreatesForOwner(smb2_context* owner) {
  if (owner == nullptr) {
    return;
  }
  for (size_t index = 0; index < kAsyncCreateQueueDepth; ++index) {
    AsyncCreate& pending = asyncCreates[index];
    if (!pending.used || pending.context != owner) {
      continue;
    }
    if (static_cast<int>(index) == activeAsyncCreate && pending.inFlight) {
      pending.context = nullptr;
      pending.cancelRequested = true;
      continue;
    }
    pending = {};
    if (asyncCreateCount != 0) {
      --asyncCreateCount;
    }
    if (activeAsyncCreate == static_cast<int>(index)) {
      activeAsyncCreate = -1;
    }
  }
}

bool SmbServer::Impl::cancelAsyncCreate(smb2_context* owner,
                                        uint64_t messageId) {
  for (AsyncCreate& pending : asyncCreates) {
    if (pending.used && pending.context == owner &&
        pending.messageId == messageId) {
      pending.cancelRequested = true;
      return true;
    }
  }
  return false;
}

void SmbServer::Impl::pollAsyncCreate() {
  if (activeAsyncCreate < 0) {
    if (activeAsyncRead >= 0 || activeAsyncWrite >= 0 ||
        activeAsyncDirectory >= 0 || activeAsyncClose >= 0 ||
        bridge.requestPending()) {
      return;
    }
    uint64_t firstSequence = UINT64_MAX;
    int firstIndex = -1;
    for (size_t index = 0; index < kAsyncCreateQueueDepth; ++index) {
      if (asyncCreates[index].used &&
          asyncCreates[index].sequence < firstSequence) {
        firstSequence = asyncCreates[index].sequence;
        firstIndex = static_cast<int>(index);
      }
    }
    if (firstIndex < 0 || oldestAsyncReadSequence() < firstSequence ||
        oldestAsyncWriteSequence() < firstSequence ||
        oldestAsyncDirectorySequence() < firstSequence ||
        oldestAsyncCloseSequence() < firstSequence) {
      return;
    }
    activeAsyncCreate = firstIndex;
  }

  const int index = activeAsyncCreate;
  AsyncCreate& pending = asyncCreates[index];
  auto clearPhysical = [&]() {
    activeSlot = -1;
    activeMode = ActiveMode::kNone;
    activeLogicalOffset = 0;
    activeVfsOffset = 0;
  };
  auto discardDetached = [&]() {
    smb2_context* const owner = pending.owner;
    pending = {};
    if (asyncCreateCount != 0) {
      --asyncCreateCount;
    }
    activeAsyncCreate = -1;
    releaseDetachedOwnerIfIdle(owner);
  };
  auto submit = [&](VfsOperation operation, const char* path) -> bool {
    if (bridge.requestPending()) {
      return false;
    }
    if (!bridge.submit(operation, path, 0)) {
      completeAsyncCreate(index, SMB2_STATUS_IO_DEVICE_ERROR);
      return false;
    }
    pending.inFlight = true;
    pending.lastProgressMs = millis();
    return true;
  };

  if (pending.inFlight) {
    if (pending.context != nullptr &&
        static_cast<uint32_t>(millis() - pending.lastProgressMs) >=
            kMutateVfsTimeoutMs) {
      smb2_context* const context = pending.context;
      if (!queueAsyncStatus(context, SMB2_CREATE, SMB2_STATUS_IO_TIMEOUT,
                            pending.messageId)) {
        smb2_close_context(context);
      }
      pending.context = nullptr;
      pending.cancelRequested = true;
    }
    VfsResult result = {};
    if (!bridge.takeResult(result)) {
      return;
    }
    pending.inFlight = false;
    pending.lastProgressMs = millis();

    if (pending.phase == AsyncCreatePhase::kStat) {
      // Эта ветка ставится в очередь до синхронного statPath(): FILE_CREATE
      // существующего каталога обязан вернуть COLLISION, а любой неуспешный
      // STAT трактуется так же, как в обычном createHandler — имени нет и
      // можно переходить к MKDIR.
      pending.phase = AsyncCreatePhase::kPrepare;
      if (result.success) {
        completeAsyncCreate(index, SMB2_STATUS_OBJECT_NAME_COLLISION);
        return;
      }
    }

    if (pending.phase == AsyncCreatePhase::kMkdir) {
      if (!result.success) {
        const uint32_t status = result.status != 0
                                    ? smbStatusFromFilex(result.status)
                                    : SMB2_STATUS_ACCESS_DENIED;
        completeAsyncCreate(index, status);
        return;
      }
      invalidateFsInfo();
      invalidateParent(pending.path);
      if (pending.context == nullptr) {
        discardDetached();
        return;
      }
      const bool queued = queueAsyncCreateReply(pending);
      smb2_context* const context = pending.context;
      pending = {};
      if (asyncCreateCount != 0) {
        --asyncCreateCount;
      }
      activeAsyncCreate = -1;
      if (!queued) {
        smb2_close_context(context);
      }
      return;
    }

    if (pending.phase == AsyncCreatePhase::kCloseCommit) {
      if (!result.success) {
        pending.phase = AsyncCreatePhase::kCloseAbort;
        if (!submit(VfsOperation::kCloseAbort, nullptr)) {
          return;
        }
        return;
      }
      clearPhysical();
      if (pending.context == nullptr) {
        discardDetached();
        return;
      }
      pending.phase = AsyncCreatePhase::kMkdir;
    } else if (pending.phase == AsyncCreatePhase::kCloseAbort) {
      if (pending.previousMode == ActiveMode::kWrite &&
          pending.previousSlot >= 0 &&
          static_cast<size_t>(pending.previousSlot) < kHandleCount &&
          handles[pending.previousSlot].used) {
        handles[pending.previousSlot].failed = true;
      }
      clearPhysical();
      if (pending.context == nullptr) {
        discardDetached();
      } else {
        completeAsyncCreate(index, SMB2_STATUS_IO_DEVICE_ERROR);
      }
      return;
    }
  }

  if (pending.context == nullptr) {
    discardDetached();
    return;
  }
  if (pending.cancelRequested) {
    completeAsyncCreate(index, SMB2_STATUS_CANCELLED);
    return;
  }
  const Tree* tree = findTree(pending.treeId);
  if (tree == nullptr || tree->owner != pending.context) {
    completeAsyncCreate(index, SMB2_STATUS_NETWORK_NAME_DELETED);
    return;
  }
  if (bridge.requestPending()) {
    return;
  }

  if (pending.phase == AsyncCreatePhase::kStat) {
    submit(VfsOperation::kStat, pending.path);
    return;
  }

  if (pending.phase == AsyncCreatePhase::kPrepare) {
    if (activeSlot >= 0 && activeMode != ActiveMode::kNone) {
      pending.previousSlot = activeSlot;
      pending.previousMode = activeMode;
      if (activeMode != ActiveMode::kDirectory) {
        pending.phase = AsyncCreatePhase::kCloseCommit;
        submit(VfsOperation::kCloseCommit, nullptr);
        return;
      }
      clearPhysical();
    }
    pending.phase = AsyncCreatePhase::kMkdir;
  }
  submit(VfsOperation::kMkdir, pending.path);
}

int SmbServer::Impl::allocateAsyncClose() const {
  if (asyncCloseCount >= kAsyncCloseQueueDepth) {
    return -1;
  }
  for (size_t index = 0; index < kAsyncCloseQueueDepth; ++index) {
    if (!asyncCloses[index].used) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

bool SmbServer::Impl::enqueueAsyncClose(smb2_context* context,
                                        uint64_t messageId, int slot,
                                        uint32_t generation, uint16_t flags) {
  const int index = allocateAsyncClose();
  if (index < 0 || context == nullptr || messageId == 0 || slot < 0 ||
      static_cast<size_t>(slot) >= kHandleCount || !handles[slot].used ||
      handles[slot].generation != generation) {
    return false;
  }
  AsyncClose& pending = asyncCloses[index];
  pending = {};
  pending.used = true;
  pending.context = context;
  pending.owner = context;
  pending.messageId = messageId;
  pending.sequence = nextAsyncVfsSequence();
  pending.generation = generation;
  pending.flags = flags;
  pending.slot = slot;
  ++asyncCloseCount;
  diagnosticLogEvent("SMB close-deferred mid=%llu slot=%d waiting=%u path=%s",
                     static_cast<unsigned long long>(messageId), slot,
                     static_cast<unsigned>(asyncCloseCount),
                     handles[slot].path);
  return true;
}

void SmbServer::Impl::completeAsyncClose(int index, uint32_t status,
                                         const smb2_close_reply* reply) {
  if (index < 0 || static_cast<size_t>(index) >= kAsyncCloseQueueDepth ||
      !asyncCloses[index].used) {
    return;
  }
  AsyncClose& pending = asyncCloses[index];
  smb2_context* const context = pending.context;
  smb2_context* const owner = pending.owner;
  const uint64_t messageId = pending.messageId;
  const bool detached = context == nullptr;
  pending = {};
  if (asyncCloseCount != 0) {
    --asyncCloseCount;
  }
  if (activeAsyncClose == index) {
    activeAsyncClose = -1;
  }

  bool queued = context == nullptr;
  if (context != nullptr && status == SMB2_STATUS_SUCCESS && reply != nullptr) {
    smb2_close_reply encoded = *reply;
    smb2_pdu* pdu =
        smb2_cmd_close_reply_async(context, &encoded, nullptr, nullptr);
    if (pdu != nullptr) {
      smb2_set_pdu_message_id(context, pdu, messageId);
      smb2_queue_pdu(context, pdu);
      queued = true;
    }
  } else if (context != nullptr) {
    queued = queueAsyncStatus(context, SMB2_CLOSE, status, messageId);
  }
  if (!queued && context != nullptr) {
    smb2_close_context(context);
  }
  if (detached) {
    releaseDetachedOwnerIfIdle(owner);
  }
}

void SmbServer::Impl::dropAsyncClosesForOwner(smb2_context* owner) {
  if (owner == nullptr) {
    return;
  }
  for (size_t index = 0; index < kAsyncCloseQueueDepth; ++index) {
    AsyncClose& pending = asyncCloses[index];
    if (!pending.used || pending.owner != owner) {
      continue;
    }
    pending = {};
    if (asyncCloseCount != 0) {
      --asyncCloseCount;
    }
    if (activeAsyncClose == static_cast<int>(index)) {
      activeAsyncClose = -1;
    }
  }
}

bool SmbServer::Impl::cancelAsyncClose(smb2_context* owner,
                                       uint64_t messageId) {
  for (AsyncClose& pending : asyncCloses) {
    if (pending.used && pending.owner == owner &&
        pending.messageId == messageId) {
      pending.cancelRequested = true;
      return true;
    }
  }
  return false;
}

void SmbServer::Impl::pollAsyncClose() {
  if (activeAsyncClose < 0) {
    for (size_t index = 0; index < kAsyncCloseQueueDepth; ++index) {
      if (asyncCloses[index].used && asyncCloses[index].cancelRequested) {
        completeAsyncClose(static_cast<int>(index), SMB2_STATUS_CANCELLED);
        return;
      }
    }
    if (activeAsyncRead >= 0 || activeAsyncWrite >= 0 ||
        activeAsyncDirectory >= 0 || activeAsyncCreate >= 0 ||
        bridge.requestPending()) {
      return;
    }
    uint64_t firstSequence = UINT64_MAX;
    int firstIndex = -1;
    for (size_t index = 0; index < kAsyncCloseQueueDepth; ++index) {
      if (asyncCloses[index].used &&
          asyncCloses[index].sequence < firstSequence) {
        firstSequence = asyncCloses[index].sequence;
        firstIndex = static_cast<int>(index);
      }
    }
    if (firstIndex < 0 || oldestAsyncReadSequence() < firstSequence ||
        oldestAsyncWriteSequence() < firstSequence ||
        oldestAsyncDirectorySequence() < firstSequence ||
        oldestAsyncCreateSequence() < firstSequence) {
      return;
    }
    activeAsyncClose = firstIndex;
  }

  const int index = activeAsyncClose;
  AsyncClose& pending = asyncCloses[index];
  if (pending.context == nullptr) {
    completeAsyncClose(index, SMB2_STATUS_CANCELLED);
    return;
  }
  if (pending.cancelRequested) {
    completeAsyncClose(index, SMB2_STATUS_CANCELLED);
    return;
  }
  if (pending.slot < 0 ||
      static_cast<size_t>(pending.slot) >= kHandleCount ||
      !handles[pending.slot].used ||
      handles[pending.slot].generation != pending.generation ||
      handles[pending.slot].owner != pending.owner) {
    completeAsyncClose(index, SMB2_STATUS_FILE_CLOSED);
    return;
  }

  smb2_close_reply reply = {};
  const uint32_t status = finalizeClose(
      pending.slot, pending.generation, pending.flags, reply);
  completeAsyncClose(index, status,
                     status == SMB2_STATUS_SUCCESS ? &reply : nullptr);
}

void SmbServer::Impl::pollAsyncDirectory() {
  if (activeAsyncDirectory < 0) {
    // Наличие READ/WRITE в очереди само по себе не запрещает каталог. Выбираем
    // старейшую физическую операцию; именно прежний запрет по asyncReadCount
    // позволял более позднему чтению EXE заморозить QUERY_DIRECTORY навсегда.
    if (activeAsyncRead >= 0 || activeAsyncWrite >= 0 ||
        activeAsyncClose >= 0 ||
        bridge.requestPending()) {
      return;
    }
    uint64_t firstSequence = UINT64_MAX;
    int firstIndex = -1;
    for (size_t index = 0; index < kAsyncDirectoryQueueDepth; ++index) {
      if (asyncDirectories[index].used &&
          asyncDirectories[index].sequence < firstSequence) {
        firstSequence = asyncDirectories[index].sequence;
        firstIndex = static_cast<int>(index);
      }
    }
    if (firstIndex < 0) {
      return;
    }
    const uint64_t readSequence = oldestAsyncReadSequence();
    const uint64_t writeSequence = oldestAsyncWriteSequence();
    const uint64_t createSequence = oldestAsyncCreateSequence();
    const uint64_t closeSequence = oldestAsyncCloseSequence();
    if (readSequence < firstSequence || writeSequence < firstSequence ||
        createSequence < firstSequence || closeSequence < firstSequence) {
      return;
    }
    activeAsyncDirectory = firstIndex;
  }

  const int index = activeAsyncDirectory;
  AsyncDirectory& pending = asyncDirectories[index];
  auto validHandle = [&]() -> Handle* {
    if (pending.slot < 0 ||
        static_cast<size_t>(pending.slot) >= kHandleCount) {
      return nullptr;
    }
    Handle& handle = handles[pending.slot];
    return handle.used && handle.owner == pending.context &&
                   handle.generation == pending.generation
               ? &handle
               : nullptr;
  };
  auto submit = [&](VfsOperation operation, const char* path) -> bool {
    if (bridge.requestPending() || !bridge.submit(operation, path, 0)) {
      completeAsyncDirectory(index, SMB2_STATUS_IO_DEVICE_ERROR);
      return false;
    }
    pending.inFlight = true;
    pending.lastProgressMs = millis();
    return true;
  };

  if (pending.inFlight) {
    if (!pending.replied &&
        static_cast<uint32_t>(millis() - pending.lastProgressMs) >=
            kNormalVfsTimeoutMs) {
      diagnosticLogEvent("SMB directory-watchdog op=%u waiting=%u",
                         static_cast<unsigned>(pending.phase),
                         static_cast<unsigned>(asyncDirectoryCount));
      // Один зависший FILEX держит общий мост, поэтому завершаем все ожидающие
      // QUERY_DIRECTORY. Иначе второй запрос того же compound снова оставит
      // Проводник без полного ответа.
      failAsyncDirectories(SMB2_STATUS_IO_TIMEOUT);
      return;
    }
    VfsResult result = {};
    if (!bridge.takeResult(result)) {
      return;
    }
    pending.inFlight = false;
    pending.lastProgressMs = millis();
    if (pending.context == nullptr || pending.replied) {
      activeSlot = -1;
      activeMode = ActiveMode::kNone;
      pending = {};
      if (asyncDirectoryCount != 0) {
        --asyncDirectoryCount;
      }
      activeAsyncDirectory = -1;
      return;
    }
    Handle* handle = validHandle();
    if (handle == nullptr) {
      completeAsyncDirectory(index, SMB2_STATUS_FILE_CLOSED);
      return;
    }

    switch (pending.phase) {
      case AsyncDirectoryPhase::kCloseCommit:
        if (!result.success) {
          pending.closeFailed = true;
          pending.phase = AsyncDirectoryPhase::kCloseAbort;
          if (!submit(VfsOperation::kCloseAbort, nullptr)) {
            return;
          }
          return;
        }
        activeSlot = -1;
        activeMode = ActiveMode::kNone;
        activeLogicalOffset = 0;
        activeVfsOffset = 0;
        pending.phase = AsyncDirectoryPhase::kOpen;
        break;

      case AsyncDirectoryPhase::kCloseAbort:
        if (pending.previousMode == ActiveMode::kWrite &&
            pending.previousSlot >= 0 &&
            static_cast<size_t>(pending.previousSlot) < kHandleCount &&
            handles[pending.previousSlot].used) {
          handles[pending.previousSlot].failed = true;
        }
        activeSlot = -1;
        activeMode = ActiveMode::kNone;
        activeLogicalOffset = 0;
        activeVfsOffset = 0;
        completeAsyncDirectory(index, SMB2_STATUS_IO_DEVICE_ERROR);
        return;

      case AsyncDirectoryPhase::kOpen:
        if (!result.success) {
          completeAsyncDirectory(index,
                                 result.status != 0
                                     ? smbStatusFromFilex(result.status)
                                     : SMB2_STATUS_IO_DEVICE_ERROR);
          return;
        }
        activeSlot = pending.slot;
        activeMode = ActiveMode::kDirectory;
        activeLogicalOffset = 0;
        activeVfsOffset = 0;
        beginDirectoryCacheBuild(pending.slot, *handle);
        pending.replayRemaining = handle->directoryIndex;
        pending.phase = pending.replayRemaining == 0
                            ? AsyncDirectoryPhase::kRead
                            : AsyncDirectoryPhase::kReplay;
        break;

      case AsyncDirectoryPhase::kReplay:
        if (!result.success) {
          completeAsyncDirectory(index, SMB2_STATUS_IO_DEVICE_ERROR);
          return;
        }
        if (result.atEnd) {
          if (directoryCacheBuildSlot == pending.slot &&
              directoryCacheBuildGeneration == handle->generation) {
            abortDirectoryCacheBuild();
          }
          handle->directoryEnded = true;
          completeAsyncDirectory(index, SMB2_STATUS_NO_MORE_FILES);
          return;
        }
        if (pending.replayRemaining != 0) {
          --pending.replayRemaining;
        }
        if (pending.replayRemaining == 0) {
          pending.phase = AsyncDirectoryPhase::kRead;
        }
        break;

      case AsyncDirectoryPhase::kRead: {
        if (!result.success) {
          completeAsyncDirectory(index,
                                 result.status != 0
                                     ? smbStatusFromFilex(result.status)
                                     : SMB2_STATUS_IO_DEVICE_ERROR);
          return;
        }
        if (result.atEnd) {
          finishDirectoryCacheBuild(pending.slot, *handle);
          handle->directoryEnded = true;
          completeAsyncDirectory(index, SMB2_STATUS_NO_MORE_FILES);
          return;
        }
        appendDirectoryCacheBuild(pending.slot, *handle, result);
        ++handle->directoryIndex;
        if (!wildcardMatch(handle->pattern, result.name)) {
          break;
        }
        const size_t encoded =
            directoryEncodedSize(pending.informationClass, result.name);
        if (encoded == 0 || encoded > pending.outputBufferLength) {
          handle->directoryPending = true;
          handle->directoryPendingIsDirectory = result.isDirectory;
          handle->directoryPendingSize = result.size;
          handle->directoryPendingIndex = handle->directoryIndex;
          snprintf(handle->directoryPendingName,
                   sizeof(handle->directoryPendingName), "%s", result.name);
          completeAsyncDirectory(index, SMB2_STATUS_BUFFER_TOO_SMALL);
          return;
        }
        const bool queued = queueAsyncDirectoryReply(
            pending, *handle, result.name, result.isDirectory, result.size,
            handle->directoryIndex);
        smb2_context* context = pending.context;
        pending = {};
        if (asyncDirectoryCount != 0) {
          --asyncDirectoryCount;
        }
        activeAsyncDirectory = -1;
        if (!queued && context != nullptr) {
          smb2_close_context(context);
        }
        return;
      }

      case AsyncDirectoryPhase::kPrepare:
        break;
    }
  }

  if (pending.cancelRequested) {
    completeAsyncDirectory(index, SMB2_STATUS_CANCELLED);
    return;
  }
  Handle* handle = validHandle();
  if (handle == nullptr) {
    completeAsyncDirectory(index, SMB2_STATUS_FILE_CLOSED);
    return;
  }
  if (!pending.prepared) {
    if ((pending.flags & (SMB2_RESTART_SCANS | SMB2_REOPEN)) != 0) {
      if (directoryCacheBuildSlot == pending.slot &&
          directoryCacheBuildGeneration == handle->generation) {
        abortDirectoryCacheBuild();
      }
      handle->directoryIndex = 0;
      handle->directoryEnded = false;
      handle->directoryPending = false;
      handle->directoryLastValid = false;
      handle->directoryCursor = {};
      if (activeSlot == pending.slot &&
          activeMode == ActiveMode::kDirectory) {
        activeSlot = -1;
        activeMode = ActiveMode::kNone;
      }
    }
    if (pending.hasName) {
      snprintf(handle->pattern, sizeof(handle->pattern), "%s", pending.name);
    }
    pending.prepared = true;
    pending.phase = AsyncDirectoryPhase::kPrepare;
  }
  if (handle->directoryEnded) {
    completeAsyncDirectory(index, SMB2_STATUS_NO_MORE_FILES);
    return;
  }
  if (handle->directoryPending) {
    const size_t encoded = directoryEncodedSize(
        pending.informationClass, handle->directoryPendingName);
    if (encoded == 0 || encoded > pending.outputBufferLength) {
      completeAsyncDirectory(index, SMB2_STATUS_BUFFER_TOO_SMALL);
      return;
    }
    const bool queued = queueAsyncDirectoryReply(
        pending, *handle, handle->directoryPendingName,
        handle->directoryPendingIsDirectory, handle->directoryPendingSize,
        handle->directoryPendingIndex);
    smb2_context* context = pending.context;
    handle->directoryPending = false;
    pending = {};
    if (asyncDirectoryCount != 0) {
      --asyncDirectoryCount;
    }
    activeAsyncDirectory = -1;
    if (!queued && context != nullptr) {
      smb2_close_context(context);
    }
    return;
  }

  // activeAsyncDirectory уже выбран общей FIFO-проверкой выше. Само наличие
  // более поздних READ/WRITE в очереди не должно снова остановить каталог:
  // иначе pollAsyncRead() уступит старшему QUERY_DIRECTORY, а каталог здесь
  // уступит READ — и оба запроса останутся ждать друг друга.
  if (bridge.requestPending()) {
    return;
  }
  if (activeSlot != pending.slot || activeMode != ActiveMode::kDirectory) {
    if (activeSlot >= 0 && activeMode != ActiveMode::kNone &&
        activeMode != ActiveMode::kDirectory) {
      pending.previousSlot = activeSlot;
      pending.previousMode = activeMode;
      pending.phase = AsyncDirectoryPhase::kCloseCommit;
      if (!submit(VfsOperation::kCloseCommit, nullptr)) {
        return;
      }
      return;
    }
    activeSlot = -1;
    activeMode = ActiveMode::kNone;
    pending.phase = AsyncDirectoryPhase::kOpen;
    if (!submit(VfsOperation::kOpenDirectory, handle->path)) {
      return;
    }
    return;
  }

  if (pending.phase != AsyncDirectoryPhase::kReplay) {
    pending.phase = AsyncDirectoryPhase::kRead;
  }
  submit(VfsOperation::kReadDirectory, nullptr);
}

int SmbServer::Impl::replyIoctlStatus(smb2_context* smb2,
                                      smb2_ioctl_reply* reply,
                                      uint32_t status) {
  if (smb2 == nullptr || reply == nullptr) {
    return -1;
  }
  smb2_pdu* pdu =
      smb2_cmd_ioctl_reply_async(smb2, reply, nullptr, nullptr);
  if (pdu == nullptr) {
    return -1;
  }
  smb2_set_pdu_status(smb2, pdu, static_cast<int>(status));
  smb2_set_pdu_message_id(smb2, pdu,
                          smb2_get_last_request_message_id(smb2));
  smb2_queue_pdu(smb2, pdu);
  return 1;
}

int SmbServer::Impl::createStatus(smb2_context* smb2,
                                  smb2_create_request* request,
                                  uint32_t status) {
  diagnosticLogEvent("SMB create-error status=%08lx name=%s",
                     static_cast<unsigned long>(status),
                     request == nullptr || request->name == nullptr
                         ? "(null)"
                         : request->name);
  // Имя запроса принадлежит libsmb2: она выделяет его при разборе CREATE и
  // освобождает вместе с PDU независимо от того, ответили мы успехом или
  // отказом. Освобождение здесь давало двойное free — на ESP это выглядело как
  // паника без следов сразу после записи «create-error» в лог, а на хосте
  // воспроизводится как access violation внутри smb2_free_data.
  return replyStatus(smb2, SMB2_CREATE, status);
}

int SmbServer::Impl::destructionHandler(smb2_server* serverValue,
                                        smb2_context* smb2) {
  Impl* self = from(serverValue);
  diagnosticLogEvent("SMB destruction client=%p known=%u", smb2,
                     self != nullptr && self->knownClient(smb2) ? 1U : 0U);
  if (self != nullptr) {
    self->cleanupClient(smb2);
  }
  return 0;
}

int SmbServer::Impl::serviceHandler(smb2_server* serverValue) {
  Impl* self = from(serverValue);
  if (self != nullptr) {
    self->pollIoInterimPending();
    if (self->asyncIoTimedOut()) {
      diagnosticLogEvent("SMB async-io-watchdog timeout=%lu",
                         static_cast<unsigned long>(
                             kAsyncIoProgressTimeoutMs));
      if (self->asyncReadCount != 0) {
        self->failAsyncReads(SMB2_STATUS_IO_TIMEOUT);
      }
      if (self->asyncWriteCount != 0) {
        self->failAsyncWrites(SMB2_STATUS_IO_TIMEOUT);
      }
      return 0;
    }
    self->pollAsyncRead();
    // После финала запускаем старейший уже принятый READ. До 30 секунд он
    // удерживает credit; более долгий получает отдельный interim PENDING.
    self->promoteQueuedReads();
    self->pollAsyncRead();
    self->pollAsyncWrite();
    self->pollAsyncDirectory();
    self->pollAsyncCreate();
    self->pollAsyncClose();
  }
  return 0;
}

int SmbServer::Impl::authorizeHandler(smb2_server* serverValue,
                                      smb2_context* smb2,
                                      const char* requestedUser,
                                      const char* requestedDomain,
                                      const char*) {
  Impl* self = from(serverValue);
  if (self == nullptr || requestedUser == nullptr) {
    diagnosticLogEvent("SMB auth-denied user=%s",
                       requestedUser == nullptr ? "(null)" : requestedUser);
    return -1;
  }
  const char* pureUser = requestedUser;
  const char* slash = strpbrk(requestedUser, "\\/");
  if (slash != nullptr) {
    pureUser = slash + 1;
  }
  if (!asciiEqualNoCase(pureUser, self->user)) {
    diagnosticLogEvent("SMB auth-denied user=%s", requestedUser);
    return -1;
  }
  diagnosticLogEvent("SMB auth-accepted user=%s pure=%s dom=%s", requestedUser,
                     pureUser,
                     requestedDomain != nullptr ? requestedDomain : "(none)");
  smb2_set_user(smb2, self->user);
  if (requestedDomain != nullptr && requestedDomain[0] != 0) {
    smb2_set_domain(smb2, requestedDomain);
  } else {
    smb2_set_domain(smb2, self->workgroup);
  }
  smb2_set_password(smb2, self->password);
  return 0;
}

int SmbServer::Impl::sessionHandler(smb2_server* serverValue,
                                    smb2_context*) {
  Impl* self = from(serverValue);
  if (self != nullptr) {
    diagnosticLogEvent("SMB session-ready");
    self->sendClientEvent(2);
    self->sendOperation("LOGIN");
  }
  return 0;
}

int SmbServer::Impl::logoffHandler(smb2_server* serverValue,
                                   smb2_context* smb2) {
  Impl* self = from(serverValue);
  if (self != nullptr && smb2 != nullptr) {
    diagnosticLogEvent("SMB logoff");
    // LOGOFF завершает сеанс, но не обязан закрывать сам TCP-сокет: соединение
    // остаётся в списке, и новый SESSION_SETUP на нём снова пройдёт обычную
    // авторизацию. Освобождаем только файлы и деревья этого соединения —
    // соседние сеансы Проводника продолжают работать.
    self->releaseClientHandles(smb2);
    self->sendClientEvent(1);
    self->sendOperation("LOGOFF");
  }
  return 0;
}

int SmbServer::Impl::treeConnectHandler(
    smb2_server* serverValue, smb2_context* smb2,
    smb2_tree_connect_request* request, smb2_tree_connect_reply* reply) {
  Impl* self = from(serverValue);
  char requestedShare[32] = {};
  if (self == nullptr || !self->parseShare(request, requestedShare)) {
    diagnosticLogEvent("SMB tree-bad-request");
    return replyStatus(smb2, SMB2_TREE_CONNECT,
                       SMB2_STATUS_BAD_NETWORK_NAME);
  }
  diagnosticLogEvent("SMB tree-request share=%s", requestedShare);
  const bool ipc = asciiEqualNoCase(requestedShare, "IPC$");
  if (ipc) {
    reply->share_type = SMB2_SHARE_TYPE_PIPE;
    reply->maximal_access = 0x001F00A9;
  } else if (asciiEqualNoCase(requestedShare, self->share)) {
    reply->share_type = SMB2_SHARE_TYPE_DISK;
    reply->maximal_access = 0x001F01FF;
  } else {
    return replyStatus(smb2, SMB2_TREE_CONNECT,
                       SMB2_STATUS_BAD_NETWORK_NAME);
  }
  // IPC$ и SD могут одновременно жить в одном SMB-сеансе. Выдаём каждому
  // TREE_CONNECT собственный TreeId и запоминаем тип именно этого дерева.
  // Глобальный флаг здесь недопустим: служебный запрос IPC$ от Windows/Linux
  // иначе превращал последующие обращения к SD в OBJECT_NAME_NOT_FOUND.
  reply->tree_id = self->allocateTree(ipc, smb2);
  if (reply->tree_id == 0) {
    return replyStatus(smb2, SMB2_TREE_CONNECT,
                       SMB2_STATUS_INSUFFICIENT_RESOURCES);
  }
  // Стандартные флаги общего ресурса Windows (MANUAL_CACHING = 0x00).
  reply->share_flags = SMB2_SHAREFLAG_MANUAL_CACHING;
  reply->capabilities = 0;
  self->sendOperation("TREE", requestedShare);
  return 0;
}

int SmbServer::Impl::treeDisconnectHandler(smb2_server* serverValue,
                                           smb2_context* smb2,
                                           uint32_t treeId) {
  Impl* self = from(serverValue);
  const Tree* tree = self == nullptr ? nullptr : self->findTree(treeId);
  if (tree == nullptr || tree->owner != smb2) {
    return replyStatus(smb2, SMB2_TREE_DISCONNECT,
                       SMB2_STATUS_NETWORK_NAME_DELETED);
  }
  if (!self->releaseTreeHandles(smb2, treeId)) {
    return replyStatus(smb2, SMB2_TREE_DISCONNECT,
                       SMB2_STATUS_IO_TIMEOUT);
  }
  self->releaseTree(treeId);
  self->sendOperation("UNTREE");
  return 0;
}

int SmbServer::Impl::createHandler(smb2_server* serverValue,
                                   smb2_context* smb2,
                                   smb2_create_request* request,
                                   smb2_create_reply* reply) {
  Impl* self = from(serverValue);
  if (self == nullptr || request == nullptr || reply == nullptr) {
    return createStatus(smb2, request, SMB2_STATUS_INVALID_PARAMETER);
  }
  RequestedCreateContexts requestedContexts;
  if (!parseRequestedCreateContexts(*request, requestedContexts)) {
    return createStatus(smb2, request, SMB2_STATUS_INVALID_PARAMETER);
  }
  diagnosticLogEvent(
      "SMB create-enter tree=%08lx disp=%lu opts=%08lx name=%s",
      static_cast<unsigned long>(smb2_get_current_tree_id(smb2)),
      static_cast<unsigned long>(request->create_disposition),
      static_cast<unsigned long>(request->create_options),
      request->name == nullptr ? "(null)" : request->name);
  const uint32_t treeId = smb2_get_current_tree_id(smb2);
  const Tree* tree = self->findTree(treeId);
  if (tree == nullptr || tree->owner != smb2) {
    return createStatus(smb2, request, SMB2_STATUS_NETWORK_NAME_DELETED);
  }
  if (tree->ipc) {
    const char* pipeName = request->name == nullptr ? "" : request->name;
    while (*pipeName == '\\' || *pipeName == '/') {
      ++pipeName;
    }
    if (!asciiEqualNoCase(pipeName, "srvsvc")) {
      return createStatus(smb2, request, SMB2_STATUS_OBJECT_NAME_NOT_FOUND);
    }
    const int slot = self->allocateHandle(smb2, treeId);
    if (slot < 0) {
      return createStatus(smb2, request,
                          SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    Handle& handle = self->handles[slot];
    handle.pipe = true;
    handle.rpcContextId = 0xFFFF;
    snprintf(handle.path, sizeof(handle.path), "srvsvc");
    memcpy(reply->file_id, handle.fileId, SMB2_FD_SIZE);
    reply->oplock_level = SMB2_OPLOCK_LEVEL_NONE;
    reply->create_action = kCreateOpened;
    reply->file_attributes = SMB2_FILE_ATTRIBUTE_NORMAL;
    self->fillCreateContextReply(
        requestedContexts, self->directoryFileId(handle.path),
        0x005A584556495043ULL, reply->change_time, 0x001F00A9UL, *reply);
    self->sendOperation("PIPE", "srvsvc");
    return 0;
  }

  char path[kMaxPath + 1];
  if (!self->normalizePath(request->name == nullptr ? "" : request->name,
                           path) ||
      request->create_disposition > SMB2_FILE_OVERWRITE_IF) {
    return createStatus(smb2, request, SMB2_STATUS_OBJECT_NAME_INVALID);
  }
  const bool explicitDirectory =
      (request->create_options & SMB2_FILE_DIRECTORY_FILE) != 0;
  const bool explicitFile =
      (request->create_options & SMB2_FILE_NON_DIRECTORY_FILE) != 0;

  // AllocationSize во всех последующих ответах обязан быть кратен размеру
  // кластера, который этот же сервер сообщает через FileFsSizeInformation.
  // Геометрию читаем до STAT/создания, чтобы при ошибке не оставить на SD
  // объект после неуспешного CREATE.
  uint8_t fsStatus = 0;
  if (!self->ensureAllocationUnit(fsStatus)) {
    return createStatus(smb2, request,
                        fsStatus != 0 ? smbStatusFromFilex(fsStatus)
                                      : SMB2_STATUS_IO_DEVICE_ERROR);
  }
  const uint64_t volumeId =
      requestedContexts.queryOnDiskId && !requestedContexts.durableReconnect
          ? self->cachedFsInfo.serial
          : 0;
  const bool physicalQueueBusy =
      self->activeAsyncRead >= 0 || self->activeAsyncWrite >= 0 ||
      self->activeAsyncDirectory >= 0 || self->activeAsyncCreate >= 0 ||
      self->activeAsyncClose >= 0 ||
      self->asyncReadCount != 0 || self->queuedReadCount != 0 ||
      self->asyncWriteCount != 0 || self->asyncDirectoryCount != 0 ||
      self->asyncCreateCount != 0 || self->asyncCloseCount != 0 ||
      self->bridge.requestPending();
  if (explicitDirectory &&
      request->create_disposition == SMB2_FILE_CREATE && physicalQueueBusy) {
    // Для нового каталога откладываем и проверку существования: statPath тоже
    // использует единственный FILEX-мост и раньше возвращал IO_TIMEOUT ещё до
    // того, как CREATE доходил до отложенного MKDIR.
    const uint64_t messageId = smb2_get_last_request_message_id(smb2);
    if (smb2_set_current_request_internal_async(smb2) != 0) {
      return -1;
    }
    if (!self->enqueueAsyncCreate(
            smb2, messageId, treeId, request->desired_access,
            request->create_options, requestedContexts, volumeId, true, path)) {
      return createStatus(smb2, request,
                          SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    self->pollAsyncCreate();
    return 1;
  }
  VfsResult statResult;
  const bool exists = self->statPath(path, statResult);
  diagnosticLogEvent(
      "SMB create-stat path=%s exists=%u dir=%u size=%lu vfs=%s", path,
      exists ? 1U : 0U, exists && statResult.isDirectory ? 1U : 0U,
      static_cast<unsigned long>(exists ? statResult.size : 0),
      self->lastVfsError);
  // Занятость канала больше не превращается в отказ: statPath отвечает из
  // памяти либо дожидается освобождения. Если бэкенд всё же недоступен, это
  // настоящая ошибка ввода-вывода, а не «повтори позже» — STATUS_RETRY здесь
  // неуместен, Windows на него не повторяет запрос, а рвёт дескриптор.
  if (!exists && strcmp(self->lastVfsError, "bridge-busy") == 0) {
    diagnosticLogEvent("SMB create-busy path=%s", path);
    return createStatus(smb2, request, SMB2_STATUS_IO_TIMEOUT);
  }
  bool directory = exists ? statResult.isDirectory : explicitDirectory;
  if (strcmp(path, "/") == 0) {
    directory = true;
  }
  if ((explicitDirectory && exists && !directory) ||
      (explicitFile && exists && directory)) {
    return createStatus(smb2, request,
                        directory ? SMB2_STATUS_FILE_IS_A_DIRECTORY
                                  : SMB2_STATUS_NOT_A_DIRECTORY);
  }

  uint32_t action = kCreateOpened;
  uint32_t size = exists ? statResult.size : 0;
  uint32_t physicalSize = size;
  bool sharedHandleFound = false;
  if (exists) {
    // statPath сообщает видимую (включая SET_EOF reserve) длину. Для нового
    // handle отдельно наследуем фактически материализованную длину: иначе
    // резерв 600001 байт ошибочно выглядит уже записанным.
    for (size_t index = 0; index < kHandleCount; ++index) {
      const Handle& shared = self->handles[index];
      if (!shared.used || !asciiEqualNoCase(shared.path, path)) {
        continue;
      }
      physicalSize = !sharedHandleFound || shared.physicalSize > physicalSize
                         ? shared.physicalSize
                         : physicalSize;
      sharedHandleFound = true;
    }
  }
  if (exists) {
    if (request->create_disposition == SMB2_FILE_CREATE) {
      return createStatus(smb2, request, SMB2_STATUS_OBJECT_NAME_COLLISION);
    }
    const bool overwrite =
        request->create_disposition == SMB2_FILE_SUPERSEDE ||
        request->create_disposition == SMB2_FILE_OVERWRITE ||
        request->create_disposition == SMB2_FILE_OVERWRITE_IF;
    if (overwrite) {
      if (directory) {
        return createStatus(smb2, request, SMB2_STATUS_FILE_IS_A_DIRECTORY);
      }
      // Без полной реализации ShareAccess безопасная семантика консервативна:
      // не усекать объект, пока хоть один handle того же пути ещё открыт.
      // Главное — не менять его physicalSize до физического truncate. Именно
      // прежний порядок превращал активный reader в size=0, а его unsigned
      // остаток — в 4 ГиБ после закономерного bridge-busy.
      for (size_t i = 0; i < kHandleCount; ++i) {
        if (self->handles[i].used &&
            asciiEqualNoCase(self->handles[i].path, path)) {
          diagnosticLogEvent("SMB overwrite-sharing path=%s slot=%u", path,
                             static_cast<unsigned>(i));
          return createStatus(smb2, request,
                              SMB2_STATUS_SHARING_VIOLATION);
        }
      }
      // Успешный CREATE с overwrite обязан уже представлять усечённый объект.
      // Отложенное до первого WRITE создание ломало второй handle: CopyFile
      // открывает его сразу после SET_EOF и начинает с позиционного хвоста.
      if (!self->createEmptyFile(path)) {
        return createStatus(smb2, request, SMB2_STATUS_ACCESS_DENIED);
      }
      size = 0;
      physicalSize = 0;
      action = request->create_disposition == SMB2_FILE_SUPERSEDE
                   ? kCreateSuperseded
                   : kCreateOverwritten;
    }
  } else {
    if (request->create_disposition == SMB2_FILE_OPEN ||
        request->create_disposition == SMB2_FILE_OVERWRITE ||
        strcmp(path, "/") == 0) {
      return createStatus(smb2, request, SMB2_STATUS_OBJECT_NAME_NOT_FOUND);
    }
    if (directory) {
      const bool mkdirQueueBusy =
          self->activeAsyncRead >= 0 || self->activeAsyncWrite >= 0 ||
          self->activeAsyncDirectory >= 0 || self->activeAsyncCreate >= 0 ||
          self->activeAsyncClose >= 0 ||
          self->asyncReadCount != 0 || self->queuedReadCount != 0 ||
          self->asyncWriteCount != 0 || self->asyncDirectoryCount != 0 ||
          self->asyncCreateCount != 0 || self->asyncCloseCount != 0 ||
          self->bridge.requestPending();
      if (mkdirQueueBusy) {
        // Проводник создаёт каталог назначения, пока следующий FINDNEXT
        // исходного каталога ещё находится на core 1. Немедленный closeActive
        // раньше превращал bridge-busy в STATUS_ACCESS_DENIED. Оставляем
        // CREATE синхронным на проводе, но исполняем MKDIR по общей FIFO.
        const uint64_t messageId = smb2_get_last_request_message_id(smb2);
        if (smb2_set_current_request_internal_async(smb2) != 0) {
          return -1;
        }
        if (!self->enqueueAsyncCreate(
                smb2, messageId, treeId, request->desired_access,
                request->create_options, requestedContexts, volumeId, false,
                path)) {
          return createStatus(smb2, request,
                              SMB2_STATUS_INSUFFICIENT_RESOURCES);
        }
        self->pollAsyncCreate();
        return 1;
      }
      VfsResult result;
      const bool created =
          self->closeActive(true) &&
          self->requestVfs(VfsOperation::kMkdir, path, 0, result,
                           kMutateVfsTimeoutMs);
      if (!created) {
        return createStatus(smb2, request, SMB2_STATUS_ACCESS_DENIED);
      }
      self->invalidateFsInfo();
      self->invalidateParent(path);
    } else if (!self->createEmptyFile(path)) {
      // CREATE должен материализовать файл до успешного ответа. Иначе другой
      // handle видит логический объект в таблице сервера, но OPEN_RANDOM на Z80
      // получает not-found — точная последовательность Windows CopyFile.
      return createStatus(smb2, request, SMB2_STATUS_ACCESS_DENIED);
    }
    size = 0;
    physicalSize = 0;
    action = kCreateCreated;
  }

  const int slot = self->allocateHandle(smb2, treeId);
  if (slot < 0) {
    return createStatus(smb2, request,
                        SMB2_STATUS_INSUFFICIENT_RESOURCES);
  }
  Handle& handle = self->handles[slot];
  handle.directory = directory;
  handle.writable =
      (request->desired_access &
       (SMB2_FILE_WRITE_DATA | SMB2_FILE_APPEND_DATA | SMB2_GENERIC_WRITE |
        SMB2_DELETE | SMB2_FILE_WRITE_ATTRIBUTES | SMB2_FILE_WRITE_EA)) != 0;
  handle.deletePending =
      (request->create_options & SMB2_FILE_DELETE_ON_CLOSE) != 0;
  handle.createdNew = (action == kCreateCreated || action == kCreateOverwritten ||
                       action == kCreateSuperseded);
  handle.physicalSize = physicalSize;
  handle.openedSize = physicalSize;
  handle.reservedSize = size;
  handle.sizeReserved = size > physicalSize;
  // Новый Open лишь наследует видимый логический EOF существующего файла.
  // Право финализировать резерв остаётся у Open, принявшего SET_INFO.
  handle.ownsSizeReservation = false;
  snprintf(handle.path, sizeof(handle.path), "%s", path);
  memcpy(reply->file_id, handle.fileId, SMB2_FD_SIZE);

  const ReportedMetadata metadata =
      self->reportedMetadata(handle.path, directory);
  reply->creation_time = metadata.creationTime;
  reply->last_access_time = metadata.lastAccessTime;
  reply->last_write_time = metadata.lastWriteTime;
  reply->change_time = metadata.changeTime;
  reply->oplock_level = SMB2_OPLOCK_LEVEL_NONE;
  reply->create_action = action;
  reply->allocation_size =
      self->reportedAllocationSize(physicalSize, directory);
  reply->end_of_file = size;
  reply->file_attributes = metadata.attributes;
  self->fillCreateContextReply(
      requestedContexts, self->directoryFileId(handle.path), volumeId,
      reply->change_time,
      directory ? 0x001F01FFUL : 0x001F019FUL, *reply);
  diagnosticLogEvent("SMB create-ok slot=%d dir=%u size=%lu path=%s", slot,
                     directory ? 1U : 0U,
                     static_cast<unsigned long>(size), path);
  self->sendOperation("OPEN", path);
  if (action == kCreateCreated) {
    self->notifyChange(
        path, SMB2_NOTIFY_CHANGE_FILE_ACTION_ADDED,
        directory ? SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_DIR_NAME
                  : SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_FILE_NAME);
  } else if (action == kCreateOverwritten || action == kCreateSuperseded) {
    self->notifyChange(
        path, SMB2_NOTIFY_CHANGE_FILE_ACTION_MODIFIED,
        SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_SIZE |
            SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_LAST_WRITE);
  }
  if (!directory) {
    // Новый файл — новый счётчик. Иначе в окне остаётся итог предыдущего.
    self->resetProgress();
  }
  return 0;
}

int SmbServer::Impl::closeHandler(smb2_server* serverValue,
                                  smb2_context* smb2,
                                  smb2_close_request* request,
                                  smb2_close_reply* reply) {
  Impl* self = from(serverValue);
  int slot = -1;
  Handle* handle = self == nullptr || request == nullptr
                       ? nullptr
                       : self->findHandle(smb2, request->file_id, &slot);
  if (handle == nullptr) {
    return replyStatus(smb2, SMB2_CLOSE, SMB2_STATUS_FILE_CLOSED);
  }

  const bool physicalQueueBusy =
      self->activeAsyncRead >= 0 || self->activeAsyncWrite >= 0 ||
      self->activeAsyncDirectory >= 0 || self->activeAsyncCreate >= 0 ||
      self->activeAsyncClose >= 0 || self->asyncReadCount != 0 ||
      self->queuedReadCount != 0 || self->asyncWriteCount != 0 ||
      self->asyncDirectoryCount != 0 || self->asyncCreateCount != 0 ||
      self->asyncCloseCount != 0 || self->bridge.requestPending();
  if (physicalQueueBusy) {
    const uint64_t messageId = smb2_get_last_request_message_id(smb2);
    const uint32_t generation = handle->generation;
    if (messageId == 0) {
      return replyStatus(smb2, SMB2_CLOSE,
                         SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    if (smb2_set_current_request_internal_async(smb2) != 0) {
      return -1;
    }
    if (!self->enqueueAsyncClose(smb2, messageId, slot, generation,
                                 request->flags)) {
      return replyStatus(smb2, SMB2_CLOSE,
                         SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    self->pollAsyncClose();
    return 1;
  }

  const uint32_t status =
      self->finalizeClose(slot, handle->generation, request->flags, *reply);
  return status == SMB2_STATUS_SUCCESS
             ? 0
             : replyStatus(smb2, SMB2_CLOSE, status);
}

uint32_t SmbServer::Impl::finalizeClose(int slot, uint32_t generation,
                                        uint16_t flags,
                                        smb2_close_reply& reply) {
  if (slot < 0 || static_cast<size_t>(slot) >= kHandleCount ||
      !handles[slot].used || handles[slot].generation != generation) {
    return SMB2_STATUS_FILE_CLOSED;
  }
  Handle& handle = handles[slot];
  if (handle.deletePending) {
    for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
      if (asyncWrites[index].used && asyncWrites[index].slot == slot &&
          asyncWrites[index].generation == generation) {
        asyncWrites[index].cancelRequested = true;
      }
    }
  }
  if (!drainAsyncWritesForHandle(slot, generation)) {
    handle.failed = true;
  }
  bool resizeFailed = false;
  if (!handle.failed && !handle.deletePending && handle.sizeReserved &&
      handle.ownsSizeReservation && !commitReservedSize(slot)) {
    // Не оставляем в ответах логический размер, который не удалось записать на
    // SD. Исходный файл при этом не удаляем: ошибка EXTEND не равна сбою
    // создания нового файла.
    handle.sizeReserved = false;
    handle.ownsSizeReservation = false;
    resizeFailed = true;
  }
  bool metadataFailed = false;
  if (!handle.failed && !handle.deletePending && handle.metadataPending &&
      !applyPendingMetadata(slot)) {
    // Ошибка времени/атрибутов не должна удалять уже полностью записанный файл.
    // CLOSE всё равно закрывает VFS, но сообщает Windows о сбое метаданных.
    handle.metadataPending = false;
    metadataFailed = true;
  }
  const bool wasActive = activeSlot == slot;
  // Обычный файл материализуется ещё в CREATE, поэтому CLOSE не должен снова
  // открывать его с усечением. Это особенно важно, когда второй handle уже
  // записал данные в тот же новый файл.
  if (wasActive && handle.writable &&
      !closeActive(!handle.failed && !handle.deletePending)) {
    handle.failed = true;
  } else if (wasActive && !handle.writable) {
    closeActive(true);
  }

  memset(&reply, 0, sizeof(reply));
  if ((flags & SMB2_CLOSE_FLAG_POSTQUERY_ATTRIB) != 0) {
    const ReportedMetadata metadata =
        reportedMetadata(handle.path, handle.directory);
    reply.flags = SMB2_CLOSE_FLAG_POSTQUERY_ATTRIB;
    reply.creation_time = metadata.creationTime;
    reply.last_access_time = metadata.lastAccessTime;
    reply.last_write_time = metadata.lastWriteTime;
    reply.change_time = metadata.changeTime;
    reply.allocation_size =
        reportedAllocationSize(handle.physicalSize, handle.directory);
    reply.end_of_file = handle.physicalSize;
    reply.file_attributes = metadata.attributes;
  }

  const bool writeFailed = handle.failed;
  bool removed = true;
  const bool shouldDelete =
      handle.deletePending || (handle.failed && handle.createdNew);
  if (shouldDelete && strcmp(handle.path, "/") != 0) {
    removed = removePath(handle.path);
  } else if (handle.metadataDirty) {
    // Снимок родителя во время записи обновлялся точечно, но окончательную
    // длину и занятое место подтверждает только CLOSE. Здесь же — то самое
    // единственное уведомление о суммарном изменении длины файла.
    invalidateParent(handle.path);
  }
  if (!handle.directory && handle.progressMode != TransferProgressMode::kNone &&
      handle.progressBytes != 0) {
    // Итоговая строка обязана пройти без ограничения частоты, иначе счётчик
    // замрёт на предпоследнем блоке и файл будет выглядеть недокопированным.
    sendProgress(handle, handle.deletePending ? "DELETE" : "DONE",
                 handle.progressBytes, true);
  }
  sendOperation(handle.deletePending ? "DELETE" : "CLOSE", handle.path);
  releaseHandle(slot);
  if (!removed) {
    return SMB2_STATUS_ACCESS_DENIED;
  }
  return resizeFailed || metadataFailed || writeFailed
             ? SMB2_STATUS_IO_DEVICE_ERROR
             : SMB2_STATUS_SUCCESS;
}

int SmbServer::Impl::flushHandler(smb2_server* serverValue,
                                  smb2_context* smb2,
                                  smb2_flush_request* request) {
  Impl* self = from(serverValue);
  int slot = -1;
  Handle* handle = self == nullptr || request == nullptr
                       ? nullptr
                       : self->findHandle(smb2, request->file_id, &slot);
  if (handle == nullptr) {
    return replyStatus(smb2, SMB2_FLUSH, SMB2_STATUS_INVALID_HANDLE);
  }
  if (!self->drainAsyncWritesForHandle(slot, handle->generation)) {
    handle->failed = true;
    return replyStatus(smb2, SMB2_FLUSH, SMB2_STATUS_IO_DEVICE_ERROR);
  }
  if (handle->sizeReserved && handle->ownsSizeReservation &&
      !self->commitReservedSize(slot)) {
    handle->sizeReserved = false;
    handle->ownsSizeReservation = false;
    return replyStatus(smb2, SMB2_FLUSH, SMB2_STATUS_IO_DEVICE_ERROR);
  }
  if (handle->metadataPending) {
    if (!self->applyPendingMetadata(slot)) {
      return replyStatus(smb2, SMB2_FLUSH, SMB2_STATUS_IO_DEVICE_ERROR);
    }
  }
  self->sendOperation("FLUSH", handle->path);
  return 0;
}

int SmbServer::Impl::readHandler(smb2_server* serverValue,
                                 smb2_context* smb2,
                                 smb2_read_request* request,
                                 smb2_read_reply* reply) {
  Impl* self = from(serverValue);
  int slot = -1;
  Handle* handle = self == nullptr || request == nullptr
                       ? nullptr
                       : self->findHandle(smb2, request->file_id, &slot);
  if (handle == nullptr || handle->directory) {
    diagnosticLogEvent("SMB read-invalid slot=%d", slot);
    return replyStatus(smb2, SMB2_READ, SMB2_STATUS_INVALID_HANDLE);
  }
  if (handle->pipe) {
    // Windows обменивается с srvsvc не только через PIPE_TRANSCEIVE. Сначала
    // она пишет DCE/RPC-запрос обычной SMB2-командой WRITE, а подготовленный
    // ответ затем забирает отдельной SMB2-командой READ. Смещение файла для
    // именованного канала смысла не имеет: читаем очередь ответа по порядку.
    if (handle->pipeResponseOffset >= handle->pipeResponseLength) {
      return replyStatus(smb2, SMB2_READ, SMB2_STATUS_PIPE_EMPTY);
    }
    const uint32_t remaining =
        handle->pipeResponseLength - handle->pipeResponseOffset;
    const uint32_t wanted = static_cast<uint32_t>(minimum64(
        request->length, static_cast<uint64_t>(remaining)));
    if (wanted == 0) {
      reply->data = nullptr;
      reply->data_length = 0;
      reply->data_remaining = remaining;
      return 0;
    }

    uint8_t* data = static_cast<uint8_t*>(malloc(wanted));
    if (data == nullptr) {
      return replyStatus(smb2, SMB2_READ,
                         SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    memcpy(data, handle->pipeResponse + handle->pipeResponseOffset, wanted);
    handle->pipeResponseOffset += wanted;
    reply->data = data;  // После отправки этот malloc освободит сама libsmb2.
    reply->data_length = wanted;
    reply->data_remaining =
        handle->pipeResponseLength - handle->pipeResponseOffset;
    diagnosticLogEvent("SRVSVC pipe-read bytes=%lu left=%lu",
                       static_cast<unsigned long>(wanted),
                       static_cast<unsigned long>(reply->data_remaining));
    if (reply->data_remaining == 0) {
      handle->pipeResponseLength = 0;
      handle->pipeResponseOffset = 0;
    }
    return 0;
  }
  if (request->offset > UINT32_MAX ||
      request->minimum_count > request->length ||
      request->minimum_count > kSmbAdvertisedReadSize) {
    return replyStatus(smb2, SMB2_READ, SMB2_STATUS_INVALID_PARAMETER);
  }
  if (self->asyncWriteCount != 0) {
    return replyStatus(smb2, SMB2_READ,
                       SMB2_STATUS_INSUFFICIENT_RESOURCES);
  }
  if (handle->sizeReserved && handle->ownsSizeReservation &&
      !self->commitReservedSize(slot)) {
    handle->sizeReserved = false;
    handle->ownsSizeReservation = false;
    return replyStatus(smb2, SMB2_READ, SMB2_STATUS_IO_DEVICE_ERROR);
  }
  const uint32_t offset = static_cast<uint32_t>(request->offset);
  diagnosticLogEvent("SMB read-enter off=%lu len=%lu size=%lu path=%s",
                     static_cast<unsigned long>(offset),
                     static_cast<unsigned long>(request->length),
                     static_cast<unsigned long>(handle->physicalSize),
                     handle->path);
  if (offset >= handle->physicalSize && request->length != 0) {
    if (self->activeSlot == slot && self->activeMode == ActiveMode::kRead) {
      self->closeActive(true);
    }
    return replyStatus(smb2, SMB2_READ, SMB2_STATUS_END_OF_FILE);
  }
  const uint32_t remaining = handle->physicalSize - offset;
  const uint32_t wanted = static_cast<uint32_t>(minimum64(
      request->length,
      minimum64(static_cast<uint64_t>(remaining),
                static_cast<uint64_t>(kSmbAdvertisedReadSize))));
  if (wanted == 0) {
    reply->data = nullptr;
    reply->data_length = 0;
    reply->data_remaining = 0;
    return 0;
  }
  if (self->fileIoConflictsWithLock(slot, offset, wanted, false)) {
    return replyStatus(smb2, SMB2_READ,
                       SMB2_STATUS_FILE_LOCK_CONFLICT);
  }

  uint8_t* cacheData = static_cast<uint8_t*>(
      heap_caps_malloc(wanted, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (cacheData == nullptr) {
    cacheData = static_cast<uint8_t*>(malloc(wanted));
  }
  if (cacheData != nullptr &&
      self->serveFromFileCache(handle->path, handle->physicalSize, offset,
                               wanted, cacheData)) {
    handle->position = offset + wanted;
    reply->data = cacheData;  // После отправки буфер освободит libsmb2.
    reply->data_length = wanted;
    reply->data_remaining = 0;
    diagnosticLogEvent("SMB read-cached bytes=%lu off=%lu path=%s",
                       static_cast<unsigned long>(wanted),
                       static_cast<unsigned long>(offset), handle->path);
    if (!self->noteTransferProgress(*handle, TransferProgressMode::kRead,
                                    offset, wanted)) {
      diagnosticLogEvent("SMB progress-range-oom mode=READ path=%s",
                         handle->path);
    }
    self->sendProgress(*handle, "READ", handle->progressBytes, false);
    self->sendOperation("READ", handle->path);
    return 0;
  }
  if (cacheData != nullptr) {
    free(cacheData);
  }

  // Последовательное чтение с нулевого смещения одновременно строит полную
  // копию небольшого файла в PSRAM. После перехода READ на асинхронную очередь
  // appendFileCache оставался без соответствующего beginFileCache, поэтому
  // повторное открытие всегда снова обращалось к Z80.
  if (offset == 0 &&
      (self->cachedFileData == nullptr ||
       self->cachedFileSize != handle->physicalSize ||
       !asciiEqualNoCase(self->cachedFilePath, handle->path) ||
       self->cachedFileFilled != 0)) {
    self->beginFileCache(handle->path, handle->physicalSize);
  }

  const uint64_t messageId = smb2_get_last_request_message_id(smb2);
  const uint64_t operationSequence = self->nextAsyncVfsSequence();
  int index = -1;
  // Не обгоняем уже поставленные в очередь READ. Пока единственный физический
  // FILEX-сервис занят, сохраняем только метаданные. До 30 секунд запросы не
  // возвращают credit; более долгий PENDING одновременно сжимает target до 1,
  // поэтому отмена не поддерживает бесконечную очередь запросами-заменами.
  if (self->queuedReadHead == nullptr) {
    index = self->allocateAsyncRead();
  }
  if (index < 0) {
    // Запрос удерживает SMB-credit и ограничивает окно Windows. Обычный
    // 120-секундный таймер PDU обязан быть отключён явно. Иначе libsmb2 удаляет
    // Request из waitqueue, а поздний финальный ответ уже не с чем
    // коррелировать — копирование навсегда остаётся ждать исчезнувший блок.
    if (smb2_set_current_request_internal_async(smb2) != 0) {
      return -1;
    }
    if (!self->enqueueAsyncRead(smb2, messageId, slot, operationSequence,
                                handle->generation, offset, wanted)) {
      return replyStatus(smb2, SMB2_READ,
                         SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    return 1;
  }

  // internal_async передаёт время жизни watchdog приложения. Если физический
  // READ не успеет за 30 секунд, serviceHandler отдельно создаст AsyncId и
  // STATUS_PENDING без повторной выдачи credit.
  if (smb2_set_current_request_internal_async(smb2) != 0) {
    return -1;
  }
  if (!self->beginAsyncRead(smb2, messageId, slot, operationSequence,
                             handle->generation, offset, wanted, millis(),
                             false, index)) {
    return -1;
  }

  self->pollAsyncRead();
  return 1;
}

int SmbServer::Impl::writeHandler(smb2_server* serverValue,
                                  smb2_context* smb2,
                                  smb2_write_request* request,
                                  smb2_write_reply* reply) {
  Impl* self = from(serverValue);
  int slot = -1;
  Handle* handle = self == nullptr || request == nullptr
                       ? nullptr
                       : self->findHandle(smb2, request->file_id, &slot);
  if (handle == nullptr || handle->directory) {
    return replyStatus(smb2, SMB2_WRITE, SMB2_STATUS_INVALID_HANDLE);
  }
  if (handle->pipe) {
    if (request->length == 0 || request->buf == nullptr) {
      return replyStatus(smb2, SMB2_WRITE, SMB2_STATUS_INVALID_PARAMETER);
    }
    if (handle->pipeResponseOffset < handle->pipeResponseLength) {
      // Новый запрос не имеет права молча уничтожить ещё не прочитанный ответ.
      diagnosticLogEvent("SRVSVC pipe-write busy left=%lu",
                         static_cast<unsigned long>(
                             handle->pipeResponseLength -
                             handle->pipeResponseOffset));
      return replyStatus(smb2, SMB2_WRITE, SMB2_STATUS_PIPE_BUSY);
    }
    handle->pipeResponseLength = 0;
    handle->pipeResponseOffset = 0;

    size_t responseLength = 0;
    smb2_dcerpc_request_info rpcInfo = {};
    const int rpcResult = smb2_dcerpc_srvsvc_reply(
        smb2, request->buf, request->length, self->share, self->hostname,
        &handle->rpcContextId, handle->pipeResponse,
        sizeof(handle->pipeResponse), &responseLength, &rpcInfo);
    logRpcExchange("WRITE", rpcInfo, rpcResult, responseLength);
    if (rpcResult != 0) {
      return replyStatus(smb2, SMB2_WRITE, rpcFailureStatus(rpcResult));
    }

    handle->pipeResponseLength = static_cast<uint32_t>(responseLength);
    reply->count = request->length;
    reply->remaining = 0;
    self->sendOperation("SHARES");
    return 0;
  }
  if (!handle->writable) {
    return replyStatus(smb2, SMB2_WRITE, SMB2_STATUS_ACCESS_DENIED);
  }
  if (request->offset > UINT32_MAX ||
      request->length > kSmbAdvertisedWriteSize ||
      request->length > UINT32_MAX -
                                static_cast<uint32_t>(request->offset) ||
      (request->length != 0 && request->buf == nullptr)) {
    return replyStatus(smb2, SMB2_WRITE, SMB2_STATUS_INVALID_PARAMETER);
  }
  const uint32_t offset = static_cast<uint32_t>(request->offset);
  if (request->length == 0) {
    reply->count = 0;
    reply->remaining = 0;
    return 0;
  }
  if (self->fileIoConflictsWithLock(slot, offset, request->length, true)) {
    return replyStatus(smb2, SMB2_WRITE,
                       SMB2_STATUS_FILE_LOCK_CONFLICT);
  }

  // Payload копируется в независимый PSRAM-слот до возврата из callback.
  // FILEX WRITE_AT сохраняет реальный 32-битный offset каждого запроса.
  const size_t window = minimum(self->bridge.ringCapacity(),
                                static_cast<size_t>(
                                    VfsClient::kTransferWindowSize));
  // Windows CopyFile не повторяет хвост короткого успешного WRITE. Поэтому весь
  // запрос копируется в PSRAM и всегда подтверждается полной длиной. Ответ
  // приходит после физической записи: освобождение слота и возврат SMB-кредита
  // происходят вместе, а до этого запрос можно отменить точным SMB CANCEL.
  const bool needsAsync =
      request->length > window || self->asyncReadCount != 0 ||
      self->queuedReadCount != 0 || self->asyncWriteCount != 0 ||
      self->asyncDirectoryCount != 0 || self->asyncCreateCount != 0 ||
      self->asyncCloseCount != 0 || self->activeAsyncRead >= 0 ||
      self->activeAsyncWrite >= 0 || self->activeAsyncDirectory >= 0 ||
      self->activeAsyncCreate >= 0 || self->activeAsyncClose >= 0 ||
      self->bridge.requestPending();
  if (needsAsync) {
    const uint64_t messageId = smb2_get_last_request_message_id(smb2);
    if (messageId == 0) {
      return replyStatus(smb2, SMB2_WRITE,
                         SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    // Request подтверждается только после физической записи и должен жить под
    // watchdog приложения, а не исчезать из waitqueue по обычному тайм-ауту
    // libsmb2.
    if (smb2_set_current_request_internal_async(smb2) != 0) {
      return -1;
    }
    const int asyncIndex = self->allocateAsyncWrite(request->length);
    if (asyncIndex < 0) {
      return replyStatus(smb2, SMB2_WRITE,
                         SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    AsyncWrite& pending = self->asyncWrites[asyncIndex];
    pending.used = true;
    pending.inFlight = false;
    pending.cancelRequested = false;
    pending.replied = false;
    pending.pendingSent = false;
    pending.writeThrough =
        (request->flags & SMB2_WRITEFLAG_WRITE_THROUGH) != 0;
    pending.context = smb2;
    pending.owner = smb2;
    pending.messageId = messageId;
    pending.sequence = self->nextAsyncVfsSequence();
    pending.slot = slot;
    pending.generation = handle->generation;
    pending.offset = offset;
    pending.length = request->length;
    pending.flushed = 0;
    pending.windowLength = 0;
    pending.requestStartedMs = millis();
    pending.lastProgressMs = millis();
    memcpy(self->asyncIoBuffers[asyncIndex], request->buf,
            request->length);
    ++self->asyncWriteCount;
    // Multi-credit порция занимает всё окно клиента. Возвращаем один credit
    // вместе с быстрым STATUS_PENDING: Windows продлевает тайм-аут операции,
    // но не может прислать следующую 512-КиБ порцию до физического финала.
    if (request->length > kSmbAdvertisedReadSize &&
        !self->queueIoInterimPending(smb2, SMB2_WRITE, messageId,
                                     pending.pendingSent)) {
      pending = {};
      if (self->asyncWriteCount != 0) {
        --self->asyncWriteCount;
      }
      self->restoreServerCreditsIfIoIdle(smb2);
      return -1;
    }
    self->pollAsyncWrite();
    return 1;
  }

  if (!self->activateWrite(slot, offset)) {
    return replyStatus(smb2, SMB2_WRITE, SMB2_STATUS_NOT_SUPPORTED);
  }

  uint32_t written = 0;
  while (written < request->length) {
    const size_t part = minimum(
        static_cast<size_t>(request->length - written), window);
    VfsResult result = {};
    if (part == 0 ||
        self->bridge.writeFromNetwork(request->buf + written, part) != part ||
        !self->requestVfsAt(VfsOperation::kWriteAt, offset + written,
                            static_cast<uint32_t>(part), result,
                            kMutateVfsTimeoutMs) ||
        result.transferred != part) {
      handle->failed = true;
      self->closeActive(false);
      return replyStatus(
          smb2, SMB2_WRITE,
          result.status != 0 ? smbStatusFromFilex(result.status)
                             : SMB2_STATUS_IO_DEVICE_ERROR);
    }
    const uint32_t progressOffset = offset + written;
    written += static_cast<uint32_t>(part);
    const uint32_t end = offset + written;
    if (end > handle->physicalSize) {
      self->updateSharedPhysicalSize(handle->path, end);
    }
    handle->metadataDirty = true;
    handle->position = end;
    self->activeLogicalOffset = end;
    self->activeVfsOffset = end;
    if (handle->sizeReserved &&
        handle->physicalSize >= handle->reservedSize) {
      handle->sizeReserved = false;
      handle->ownsSizeReservation = false;
    }
    if (!self->noteTransferProgress(*handle, TransferProgressMode::kWrite,
                                    progressOffset,
                                    static_cast<uint32_t>(part))) {
      diagnosticLogEvent("SMB progress-range-oom mode=WRITE path=%s",
                         handle->path);
    }
    self->sendProgress(*handle, "WRITE", handle->progressBytes, false);
  }
  reply->count = request->length;
  reply->remaining = 0;
  self->sendOperation("WRITE", handle->path);
  self->notifyChange(handle->path,
                     SMB2_NOTIFY_CHANGE_FILE_ACTION_MODIFIED,
                     SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_SIZE |
                         SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_LAST_WRITE);
  return 0;
}

int SmbServer::Impl::oplockHandler(
    smb2_server*, smb2_context*, smb2_oplock_break_acknowledgement*) {
  return 0;
}

int SmbServer::Impl::leaseHandler(
    smb2_server*, smb2_context*, smb2_lease_break_acknowledgement*) {
  return 0;
}

int SmbServer::Impl::lockHandler(smb2_server* serverValue,
                                 smb2_context* smb2,
                                 smb2_lock_request* request) {
  Impl* self = from(serverValue);
  int slot = -1;
  Handle* handle = self == nullptr || request == nullptr
                       ? nullptr
                       : self->findHandle(smb2, request->file_id, &slot);
  if (handle == nullptr) {
    return replyStatus(smb2, SMB2_LOCK, SMB2_STATUS_FILE_CLOSED);
  }
  if (handle->directory || request->lock_count == 0 ||
      request->locks == nullptr) {
    return replyStatus(smb2, SMB2_LOCK,
                       SMB2_STATUS_INVALID_PARAMETER);
  }

  const bool unlockRequest =
      (request->locks[0].flags & kLockFlagUnlock) != 0;
  const uint64_t fileKey = self->directoryFileId(handle->path);
  size_t added[kByteRangeLockCount] = {};
  size_t addedCount = 0;

  for (uint16_t index = 0; index < request->lock_count; ++index) {
    const smb2_lock_element& element = request->locks[index];
    if (element.length != 0 &&
        element.offset + element.length - 1 < element.offset) {
      return replyStatus(smb2, SMB2_LOCK,
                         SMB2_STATUS_INVALID_PARAMETER);
    }

    if (unlockRequest) {
      if (element.flags != kLockFlagUnlock) {
        return replyStatus(smb2, SMB2_LOCK,
                           SMB2_STATUS_INVALID_PARAMETER);
      }
      int found = -1;
      for (size_t lockIndex = 0; lockIndex < kByteRangeLockCount;
           ++lockIndex) {
        const ByteRangeLock& lock = self->byteRangeLocks[lockIndex];
        if (!lock.used || lock.fileKey != fileKey ||
            lock.ownerSlot != static_cast<uint16_t>(slot) ||
            lock.ownerGeneration != handle->generation ||
            lock.offset != element.offset || lock.length != element.length) {
          continue;
        }
        found = static_cast<int>(lockIndex);
        if (lock.exclusive) {
          break;
        }
      }
      if (found < 0) {
        return replyStatus(smb2, SMB2_LOCK,
                           SMB2_STATUS_RANGE_NOT_LOCKED);
      }
      self->byteRangeLocks[found] = {};
      continue;
    }

    const uint32_t kind =
        element.flags & (kLockFlagShared | kLockFlagExclusive);
    const uint32_t allowed = kind | (element.flags & kLockFlagFailImmediately);
    if ((kind != kLockFlagShared && kind != kLockFlagExclusive) ||
        element.flags != allowed ||
        (request->lock_count > 1 &&
         (element.flags & kLockFlagFailImmediately) == 0)) {
      return replyStatus(smb2, SMB2_LOCK,
                         SMB2_STATUS_INVALID_PARAMETER);
    }
    const bool exclusive = kind == kLockFlagExclusive;
    if (self->lockRangeConflicts(fileKey, slot, handle->generation,
                                 element.offset, element.length,
                                 exclusive)) {
      // Для конфликтующего FAIL_IMMEDIATELY MS-SMB2 требует снять диапазоны,
      // уже добавленные этой же составной командой.
      for (size_t rollback = 0; rollback < addedCount; ++rollback) {
        self->byteRangeLocks[added[rollback]] = {};
      }
      return replyStatus(smb2, SMB2_LOCK,
                         SMB2_STATUS_LOCK_NOT_GRANTED);
    }

    size_t freeIndex = kByteRangeLockCount;
    for (size_t lockIndex = 0; lockIndex < kByteRangeLockCount;
         ++lockIndex) {
      if (!self->byteRangeLocks[lockIndex].used) {
        freeIndex = lockIndex;
        break;
      }
    }
    if (freeIndex == kByteRangeLockCount) {
      return replyStatus(smb2, SMB2_LOCK,
                         SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    ByteRangeLock& lock = self->byteRangeLocks[freeIndex];
    lock.used = true;
    lock.exclusive = exclusive;
    lock.ownerSlot = static_cast<uint16_t>(slot);
    lock.ownerGeneration = handle->generation;
    lock.fileKey = fileKey;
    lock.offset = element.offset;
    lock.length = element.length;
    added[addedCount++] = freeIndex;
  }
  return 0;
}

int SmbServer::Impl::ioctlHandler(smb2_server* serverValue,
                                  smb2_context* smb2,
                                  smb2_ioctl_request* request,
                                  smb2_ioctl_reply* reply) {
  Impl* self = from(serverValue);
  uint32_t status = SMB2_STATUS_NOT_SUPPORTED;
  if (request != nullptr) {
    diagnosticLogEvent("SMB ioctl code=%08lx in=%lu out_max=%lu",
                       static_cast<unsigned long>(request->ctl_code),
                       static_cast<unsigned long>(request->input_count),
                       static_cast<unsigned long>(request->max_output_response));
  }
  const bool fsctlRequest =
      request != nullptr && request->flags == SMB2_0_IOCTL_IS_FSCTL;
  if (request != nullptr && !fsctlRequest) {
    status = SMB2_STATUS_NOT_SUPPORTED;
  }
  if (fsctlRequest &&
      (request->ctl_code == SMB2_FSCTL_DFS_GET_REFERRALS ||
       request->ctl_code == SMB2_FSCTL_DFS_GET_REFERRALS_EX)) {
    // Проводник сначала спрашивает IPC$, не является ли обычный UNC-путь
    // пространством DFS. По MS-SMB2 сервер без DFS обязан ответить именно
    // STATUS_FS_DRIVER_REQUIRED. Общий STATUS_NOT_SUPPORTED Windows возвращает
    // вызывающей программе и до TREE_CONNECT ресурса SD уже не доходит.
    status = isCompoundFileId(request->file_id)
                 ? SMB2_STATUS_FS_DRIVER_REQUIRED
                 : SMB2_STATUS_INVALID_PARAMETER;
  }
  if (fsctlRequest &&
      request->ctl_code == FSCTL_CREATE_OR_GET_OBJECT_ID) {
    // VFS-мост не хранит Object ID. Для поддерживаемого FSCTL на файловой
    // системе без Object ID MS-FSCC требует STATUS_INVALID_DEVICE_REQUEST.
    status = SMB2_STATUS_INVALID_DEVICE_REQUEST;
  }
  if (fsctlRequest &&
      request->ctl_code == FSCTL_QUERY_FILE_REGIONS) {
    // FAT не хранит карту valid-data regions, а поддержка этого запроса
    // необязательна. MS-FSA 2.1.5.10.24 требует от такой файловой системы
    // STATUS_INVALID_DEVICE_REQUEST. STATUS_NOT_SUPPORTED заставляет Windows
    // CopyFile повторять оптимизационный запрос и не переходить к обычному
    // блочному копированию.
    status = SMB2_STATUS_INVALID_DEVICE_REQUEST;
  }
  if (self != nullptr && fsctlRequest && reply != nullptr &&
      request->ctl_code == SMB2_FSCTL_PIPE_WAIT) {
    // Канал srvsvc создаётся мгновенно, поэтому Windows не нужен
    // настоящий список ожидания именованных каналов.
    status = isCompoundFileId(request->file_id)
                 ? SMB2_STATUS_SUCCESS
                 : SMB2_STATUS_INVALID_PARAMETER;
  }
  if (self != nullptr && fsctlRequest && reply != nullptr &&
      request->ctl_code == SMB2_FSCTL_PIPE_TRANSCEIVE) {
    Handle* handle = self->findHandle(smb2, request->file_id);
    if (handle == nullptr || !handle->pipe) {
      status = SMB2_STATUS_FILE_CLOSED;
      diagnosticLogEvent("SRVSVC invalid-handle");
    } else if (request->input == nullptr || request->input_count == 0) {
      status = SMB2_STATUS_INVALID_PARAMETER;
    } else if (handle->pipeResponseOffset < handle->pipeResponseLength) {
      // WRITE/READ и TRANSCEIVE сериализуются на одном экземпляре канала.
      // Недочитанный ответ нельзя заменить ответом от другого RPC.
      status = SMB2_STATUS_PIPE_BUSY;
      diagnosticLogEvent("SRVSVC transceive busy left=%lu",
                         static_cast<unsigned long>(
                             handle->pipeResponseLength -
                             handle->pipeResponseOffset));
    } else {
      handle->pipeResponseLength = 0;
      handle->pipeResponseOffset = 0;
      size_t responseLength = 0;
      smb2_dcerpc_request_info rpcInfo = {};
      const int rpcResult = smb2_dcerpc_srvsvc_reply(
          smb2, static_cast<const uint8_t*>(request->input),
          request->input_count, self->share, self->hostname,
          &handle->rpcContextId,
          handle->pipeResponse, sizeof(handle->pipeResponse),
          &responseLength, &rpcInfo);
      logRpcExchange("IOCTL", rpcInfo, rpcResult, responseLength);
      if (rpcResult == 0) {
        const size_t outputLength = minimum(
            responseLength,
            static_cast<size_t>(request->max_output_response));
        reply->output = outputLength == 0 ? nullptr : handle->pipeResponse;
        reply->output_count = static_cast<uint32_t>(outputLength);
        status = outputLength < responseLength
                     ? SMB2_STATUS_BUFFER_OVERFLOW
                     : SMB2_STATUS_SUCCESS;
        self->sendOperation("SHARES");
      } else {
        status = rpcFailureStatus(rpcResult);
      }
    }
  }
  // Входные данные IOCTL освобождает сама libsmb2 (libsmb2.c: smb2_free_data
  // по req->input при разборе команды). Повторное освобождение здесь давало
  // двойное free — на ESP это и была паника без следов.
  if (status == SMB2_STATUS_SUCCESS) {
    // Нулевой return просит libsmb2 собрать нормальный SMB2 IOCTL Response из
    // заполненного reply. Нельзя посылать SMB2 ERROR с кодом SUCCESS: формально
    // статус зелёный, но структура пакета другая, и Windows отвергает её.
    return 0;
  }
  if (status == SMB2_STATUS_BUFFER_OVERFLOW) {
    // Для PIPE_TRANSCEIVE warning STATUS_BUFFER_OVERFLOW всё равно обязан
    // иметь обычную структуру IOCTL Response с доступной частью данных.
    diagnosticLogEvent("SMB ioctl-overflow bytes=%lu",
                       static_cast<unsigned long>(reply->output_count));
    return replyIoctlStatus(smb2, reply, status);
  }
  diagnosticLogEvent("SMB ioctl-error status=%08lx",
                     static_cast<unsigned long>(status));
  return replyStatus(smb2, SMB2_IOCTL, status);
}

int SmbServer::Impl::cancelHandler(smb2_server* serverValue,
                                   smb2_context* smb2) {
  // По протоколу SMB2 CANCEL не имеет отдельного ответа.
  Impl* self = from(serverValue);
  if (self == nullptr || smb2 == nullptr) {
    return 0;
  }

  smb2_pdu* target = nullptr;
  if ((smb2->hdr.flags & SMB2_FLAGS_ASYNC_COMMAND) != 0) {
    // После interim STATUS_PENDING Windows ставит MessageId=0 и адресует
    // отмену через AsyncId. Ищем ровно соответствующий исходный Request.
    for (smb2_pdu* request = smb2->waitqueue; request != nullptr;
         request = request->next) {
      if ((request->header.flags & SMB2_FLAGS_ASYNC_COMMAND) != 0 &&
          request->header.async.async_id == smb2->hdr.async.async_id) {
        target = request;
        break;
      }
    }
  } else {
    target = smb2_find_pdu(smb2, smb2->hdr.message_id);
  }
  if (target == nullptr) {
    return 0;
  }

  const uint64_t messageId = target->header.message_id;
  if (self->cancelNotify(smb2, messageId)) {
    return 0;
  }
  if (self->cancelAsyncDirectory(smb2, messageId)) {
    return 0;
  }
  if (self->cancelAsyncCreate(smb2, messageId)) {
    return 0;
  }
  if (self->cancelAsyncClose(smb2, messageId)) {
    return 0;
  }
  if (self->cancelQueuedRead(smb2, messageId)) {
    return 0;
  }
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    AsyncRead& read = self->asyncReads[index];
    if (read.used && read.context == smb2 &&
        read.messageId == messageId) {
      read.cancelRequested = true;
      return 0;
    }
    AsyncWrite& pending = self->asyncWrites[index];
    if (pending.used && pending.context == smb2 &&
        pending.messageId == messageId && !pending.replied) {
      // Уже выполняющийся APPEND нельзя оборвать посередине UART-кадра.
      // После ACK текущего окна отменяем ровно найденную SMB-команду.
      pending.cancelRequested = true;
      return 0;
    }
  }
  return 0;
}

int SmbServer::Impl::echoHandler(smb2_server*, smb2_context*) {
  return 0;
}

int SmbServer::Impl::queryCachedDirectory(
    smb2_context* smb2, Handle& handle,
    smb2_query_directory_request* request,
    smb2_query_directory_reply* reply) {
  if (!directoryCache.cursorCurrent(handle.directoryCursor)) {
    if (!directoryCache.openCursor(handle.path, handle.directoryCursor)) {
      return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                         SMB2_STATUS_IO_DEVICE_ERROR);
    }
    // После инвалидации общего revision восстанавливаем сырую позицию этого
    // HANDLE. В directoryIndex входят и записи, отсеянные маской Windows.
    DirectoryCache::EntryView skipped;
    for (uint32_t index = 0; index < handle.directoryIndex; ++index) {
      if (!directoryCache.next(handle.directoryCursor, skipped)) {
        handle.directoryEnded = true;
        return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                           SMB2_STATUS_NO_MORE_FILES);
      }
    }
  }

  const size_t stride = padTo8(sizeof(smb2_fileidbothdirectoryinformation));
  uint8_t* entries = directoryBatch;
  size_t capacity = kDirectoryBatchCapacity;
  if (entries == nullptr) {
    // Нехватка PSRAM не ломает перечисление: остаётся прежний ответ по одной
    // записи, но всё равно без повторного UART-чтения каталога.
    entries = reinterpret_cast<uint8_t*>(&directoryInfo);
    capacity = 1;
  }

  const size_t room = minimum(request->output_buffer_length,
                              static_cast<uint32_t>(kSmbAdvertisedTransactSize));
  size_t encoded = 0;
  size_t count = 0;
  bool reachedEnd = false;
  while (count < capacity) {
    DirectoryCache::Cursor nextCursor = handle.directoryCursor;
    DirectoryCache::EntryView cached;
    if (!directoryCache.next(nextCursor, cached)) {
      reachedEnd = true;
      break;
    }
    if (!wildcardMatch(handle.pattern, cached.name)) {
      handle.directoryCursor = nextCursor;
      ++handle.directoryIndex;
      continue;
    }

    const size_t entrySize =
        directoryEncodedSize(request->file_information_class, cached.name);
    if (entrySize == 0 || entrySize > room - encoded) {
      if (count == 0) {
        return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                           SMB2_STATUS_BUFFER_TOO_SMALL);
      }
      break;
    }

    handle.directoryCursor = nextCursor;
    ++handle.directoryIndex;
    auto* info = reinterpret_cast<smb2_fileidbothdirectoryinformation*>(
        entries + count * stride);
    fillDirectoryInfo(*info, handle.directoryIndex, cached.isDirectory,
                      cached.size, handle.path, cached.name);
    encoded += entrySize;
    ++count;
    if ((request->flags & SMB2_RETURN_SINGLE_ENTRY) != 0) {
      break;
    }
  }

  if (reachedEnd) {
    handle.directoryEnded = true;
  }
  if (count == 0) {
    return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                       SMB2_STATUS_NO_MORE_FILES);
  }

  reply->output_buffer = entries;
  // Внутренний интерфейс libsmb2 принимает массив одинаковых C-структур и
  // сам превращает его в записи переменной длины UTF-16.
  reply->output_buffer_length = static_cast<uint32_t>(count * stride);
  sendOperation("DIR", handle.path);
  return 0;
}

int SmbServer::Impl::queryStreamedDirectory(
    smb2_context* smb2, Handle& handle, int slot,
    smb2_query_directory_request* request,
    smb2_query_directory_reply* reply) {
  VfsResult result;
  if (!activateDirectory(slot)) {
    return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                       SMB2_STATUS_IO_DEVICE_ERROR);
  }
  while (true) {
    if (!requestVfs(VfsOperation::kReadDirectory, nullptr, 0, result,
                    kNormalVfsTimeoutMs)) {
      return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                         SMB2_STATUS_IO_DEVICE_ERROR);
    }
    if (result.atEnd) {
      finishDirectoryCacheBuild(slot, handle);
      handle.directoryEnded = true;
      return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                         SMB2_STATUS_NO_MORE_FILES);
    }
    appendDirectoryCacheBuild(slot, handle, result);
    ++handle.directoryIndex;
    if (wildcardMatch(handle.pattern, result.name)) {
      break;
    }
  }

  const size_t encodedEstimate =
      directoryEncodedSize(request->file_information_class, result.name);
  if (request->output_buffer_length < encodedEstimate) {
    return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                       SMB2_STATUS_BUFFER_TOO_SMALL);
  }

  memset(&directoryInfo, 0, sizeof(directoryInfo));
  snprintf(directoryName, sizeof(directoryName), "%s", result.name);
  fillDirectoryInfo(directoryInfo, handle.directoryIndex,
                    result.isDirectory, result.size,
                    handle.path, directoryName);
  reply->output_buffer = reinterpret_cast<uint8_t*>(&directoryInfo);
  reply->output_buffer_length =
      static_cast<uint32_t>(padTo8(sizeof(directoryInfo)));
  sendOperation("DIR", handle.path);
  return 0;
}

int SmbServer::Impl::queryDirectoryHandler(
    smb2_server* serverValue, smb2_context* smb2,
    smb2_query_directory_request* request,
    smb2_query_directory_reply* reply) {
  Impl* self = from(serverValue);
  int slot = -1;
  Handle* handle = self == nullptr || request == nullptr
                       ? nullptr
                       : self->findHandle(smb2, request->file_id, &slot);
  if (handle == nullptr) {
    // [MS-SMB2] 3.3.5.18: отсутствующий или не совпавший открытый объект — это
    // FILE_CLOSED. NOT_A_DIRECTORY здесь скрывал ошибку FileId/TreeId.
    return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                       SMB2_STATUS_FILE_CLOSED);
  }
  if (!handle->directory) {
    return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                       SMB2_STATUS_INVALID_PARAMETER);
  }
  // Сервер объявляет MaxTransactSize=64 КиБ. [MS-SMB2] 3.3.5.18 разрешает
  // отклонить больший запрос; молча обрезать клиентский максимум нельзя,
  // потому что CreditCharge был рассчитан для исходной длины.
  if (request->output_buffer_length > kSmbAdvertisedTransactSize) {
    return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                       SMB2_STATUS_INVALID_PARAMETER);
  }
  switch (request->file_information_class) {
    case SMB2_FILE_FULL_DIRECTORY_INFORMATION:
    case SMB2_FILE_BOTH_DIRECTORY_INFORMATION:
    case SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION:
    case SMB2_FILE_ID_FULL_DIRECTORY_INFORMATION:
      break;
    // Эти классы разрешены QUERY_DIRECTORY, но текущий кодировщик libsmb2 их
    // не сериализует. По [MS-SMB2] для разрешённого, но не реализованного
    // класса требуется NOT_SUPPORTED, а не INVALID_PARAMETER.
    case SMB2_FILE_DIRECTORY_INFORMATION:
    case SMB2_FILE_NAMES_INFORMATION:
    case 0x3c:  // FileIdExtdDirectoryInformation
    case 0x4e:  // FileId64ExtdDirectoryInformation
    case 0x4f:  // FileId64ExtdBothDirectoryInformation
    case 0x50:  // FileIdAllExtdDirectoryInformation
    case 0x51:  // FileIdAllExtdBothDirectoryInformation
      return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                         SMB2_STATUS_NOT_SUPPORTED);
    default:
      return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                         SMB2_STATUS_INVALID_INFO_CLASS);
  }

  if (self->directoryCache.contains(handle->path)) {
    // Готовый снимок не обращается к EVO и потому может быть обработан сразу.
    // Состояние запроса меняем здесь; холодные запросы применяют те же поля в
    // pollAsyncDirectory() строго в порядке очереди compound-команд.
    if ((request->flags & (SMB2_RESTART_SCANS | SMB2_REOPEN)) != 0) {
      handle->directoryIndex = 0;
      handle->directoryEnded = false;
      handle->directoryPending = false;
      handle->directoryCursor = {};
      if (self->activeSlot == slot &&
          self->activeMode == ActiveMode::kDirectory) {
        self->activeSlot = -1;
        self->activeMode = ActiveMode::kNone;
      }
    }
    if (request->name != nullptr && request->name[0] != 0) {
      snprintf(handle->pattern, sizeof(handle->pattern), "%s", request->name);
    }
    if (handle->directoryEnded) {
      return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                         SMB2_STATUS_NO_MORE_FILES);
    }
    return self->queryCachedDirectory(smb2, *handle, request, reply);
  }

  // Холодный FILEX-путь нельзя исполнять внутри callback сетевого стека:
  // медленный FINDNEXT блокировал ECHO и весь compound. Запрос остаётся в
  // waitqueue libsmb2, а pollAsyncDirectory() возвращает ровно один результат
  // после физического ответа EVO. Первый полный проход одновременно строит
  // снимок; второго, параллельного обхода каталога нет.
  const uint64_t messageId = smb2_get_last_request_message_id(smb2);
  // На проводе QUERY_DIRECTORY остаётся синхронным и не получает interim-
  // STATUS_PENDING. Время жизни Request всё равно принадлежит этой очереди и
  // её отдельному watchdog, иначе медленный каталог исчезнет из waitqueue.
  if (smb2_set_current_request_internal_async(smb2) != 0) {
    return -1;
  }
  if (!self->enqueueAsyncDirectory(smb2, messageId, slot,
                                   handle->generation, *request)) {
    return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                       SMB2_STATUS_INSUFFICIENT_RESOURCES);
  }
  self->pollAsyncDirectory();
  return 1;
}

int SmbServer::Impl::changeNotifyHandler(
    smb2_server* serverValue, smb2_context* smb2,
    smb2_change_notify_request* request, smb2_change_notify_reply*) {
  Impl* self = from(serverValue);
  int slot = -1;
  Handle* handle = self == nullptr || request == nullptr
                       ? nullptr
                       : self->findHandle(smb2, request->file_id, &slot);
  if (handle == nullptr) {
    return replyStatus(smb2, SMB2_CHANGE_NOTIFY,
                       SMB2_STATUS_INVALID_HANDLE);
  }
  if (!handle->directory || request->output_buffer_length < 12 ||
      request->completion_filter == 0 ||
      (request->flags & ~SMB2_CHANGE_NOTIFY_WATCH_TREE) != 0) {
    return replyStatus(smb2, SMB2_CHANGE_NOTIFY,
                       SMB2_STATUS_INVALID_PARAMETER);
  }

  PendingNotify* pending = nullptr;
  for (PendingNotify& candidate : self->pendingNotifies) {
    if (!candidate.used) {
      pending = &candidate;
      break;
    }
  }
  const uint64_t messageId = smb2_get_last_request_message_id(smb2);
  if (pending == nullptr || messageId == 0) {
    return replyStatus(smb2, SMB2_CHANGE_NOTIFY,
                       SMB2_STATUS_INSUFFICIENT_RESOURCES);
  }

  *pending = {};
  pending->used = true;
  pending->context = smb2;
  pending->messageId = messageId;
  pending->slot = slot;
  pending->generation = handle->generation;
  pending->completionFilter = request->completion_filter;
  pending->outputBufferLength = request->output_buffer_length;
  pending->watchTree =
      (request->flags & SMB2_CHANGE_NOTIFY_WATCH_TREE) != 0;
  snprintf(pending->path, sizeof(pending->path), "%s", handle->path);

  // Как у Samba: сначала отдельный async STATUS_PENDING. Финал придёт после
  // операции этого SMB-сервера либо STATUS_CANCELLED по SMB2 CANCEL.
  if (!self->queueAsyncStatus(smb2, SMB2_CHANGE_NOTIFY,
                              SMB2_STATUS_PENDING, messageId)) {
    *pending = {};
    return -1;
  }
  self->sendOperation("WATCH", handle->path);
  return 1;
}

int SmbServer::Impl::queryInfoHandler(smb2_server* serverValue,
                                      smb2_context* smb2,
                                      smb2_query_info_request* request,
                                      smb2_query_info_reply* reply) {
  Impl* self = from(serverValue);
  Handle* handle = self == nullptr || request == nullptr
                       ? nullptr
                       : self->findHandle(smb2, request->file_id);
  if (handle == nullptr) {
    diagnosticLogEvent("SMB query-info invalid type=%u class=%u",
                       request == nullptr ? 0U : request->info_type,
                       request == nullptr ? 0U : request->file_info_class);
    return replyStatus(smb2, SMB2_QUERY_INFO, SMB2_STATUS_INVALID_HANDLE);
  }
  diagnosticLogEvent("SMB query-info type=%u class=%u out=%lu path=%s",
                     request->info_type, request->file_info_class,
                     static_cast<unsigned long>(request->output_buffer_length),
                      handle->path);
  const uint32_t size = self->visibleSize(*handle);
  const ReportedMetadata metadata =
      self->reportedMetadata(handle->path, handle->directory);
  const uint32_t attributes = metadata.attributes;
  void* output = nullptr;
  uint32_t outputLength = 0;

  if (request->info_type == SMB2_0_INFO_FILE) {
    switch (request->file_info_class) {
      case SMB2_FILE_BASIC_INFORMATION: {
        memset(&self->basicInfo, 0, sizeof(self->basicInfo));
        smb2_win_to_timeval(metadata.creationTime,
                            &self->basicInfo.creation_time);
        smb2_win_to_timeval(metadata.lastAccessTime,
                            &self->basicInfo.last_access_time);
        smb2_win_to_timeval(metadata.lastWriteTime,
                            &self->basicInfo.last_write_time);
        smb2_win_to_timeval(metadata.changeTime,
                            &self->basicInfo.change_time);
        self->basicInfo.file_attributes = attributes;
        output = &self->basicInfo;
        outputLength = sizeof(self->basicInfo);
        break;
      }
      case SMB2_FILE_STANDARD_INFORMATION:
        memset(&self->standardInfo, 0, sizeof(self->standardInfo));
        self->standardInfo.allocation_size = self->reportedAllocationSize(
            handle->physicalSize, handle->directory);
        self->standardInfo.end_of_file = size;
        self->standardInfo.number_of_links = 1;
        self->standardInfo.delete_pending = handle->deletePending;
        self->standardInfo.directory = handle->directory;
        output = &self->standardInfo;
        outputLength = sizeof(self->standardInfo);
        break;
      case SMB2_FILE_EA_INFORMATION:
        // FAT не хранит расширенные атрибуты. Windows всё равно запрашивает
        // FILE_EA_INFORMATION перед открытием файла в Блокноте.
        memset(&self->eaInfo, 0, sizeof(self->eaInfo));
        self->eaInfo.ea_size = 0;
        output = &self->eaInfo;
        outputLength = sizeof(self->eaInfo);
        break;
      case SMB2_FILE_ALL_INFORMATION: {
        memset(&self->allInfo, 0, sizeof(self->allInfo));
        if (!self->makeInfoName(*handle)) {
          break;
        }
        smb2_win_to_timeval(metadata.creationTime,
                            &self->allInfo.basic.creation_time);
        smb2_win_to_timeval(metadata.lastAccessTime,
                            &self->allInfo.basic.last_access_time);
        smb2_win_to_timeval(metadata.lastWriteTime,
                            &self->allInfo.basic.last_write_time);
        smb2_win_to_timeval(metadata.changeTime,
                            &self->allInfo.basic.change_time);
        self->allInfo.basic.file_attributes = attributes;
        self->allInfo.standard.allocation_size =
            self->reportedAllocationSize(handle->physicalSize,
                                         handle->directory);
        self->allInfo.standard.end_of_file = size;
        self->allInfo.standard.number_of_links = 1;
        self->allInfo.standard.delete_pending = handle->deletePending;
        self->allInfo.standard.directory = handle->directory;
        self->allInfo.index_number = self->directoryFileId(handle->path);
        self->allInfo.access_flags = 0x001F01FF;
        self->allInfo.current_byte_offset = handle->position;
        self->allInfo.name =
            reinterpret_cast<const uint8_t*>(self->infoName);
        output = &self->allInfo;
        outputLength = sizeof(self->allInfo);
        break;
      }
      case SMB2_FILE_INTERNAL_INFORMATION:
        memset(&self->internalInfo, 0, sizeof(self->internalInfo));
        self->internalInfo.index_number = self->directoryFileId(handle->path);
        output = &self->internalInfo;
        outputLength = sizeof(self->internalInfo);
        break;
      case SMB2_FILE_NETWORK_OPEN_INFORMATION: {
        memset(&self->networkInfo, 0, sizeof(self->networkInfo));
        smb2_win_to_timeval(metadata.creationTime,
                            &self->networkInfo.creation_time);
        smb2_win_to_timeval(metadata.lastAccessTime,
                            &self->networkInfo.last_access_time);
        smb2_win_to_timeval(metadata.lastWriteTime,
                            &self->networkInfo.last_write_time);
        smb2_win_to_timeval(metadata.changeTime,
                            &self->networkInfo.change_time);
        self->networkInfo.allocation_size = self->reportedAllocationSize(
            handle->physicalSize, handle->directory);
        self->networkInfo.end_of_file = size;
        self->networkInfo.file_attributes = attributes;
        output = &self->networkInfo;
        outputLength = sizeof(self->networkInfo);
        break;
      }
      case SMB2_FILE_NAME_INFORMATION:
      case SMB2_FILE_NORMALIZED_NAME_INFORMATION:
        memset(&self->nameInfo, 0, sizeof(self->nameInfo));
        if (!self->makeInfoName(*handle)) {
          break;
        }
        // Длину UTF-16 вычислит кодировщик libsmb2. Умножать длину UTF-8 на
        // два нельзя: для русского имени это дало бы лишние нулевые символы.
        self->nameInfo.file_name_length = 0;
        self->nameInfo.name =
            reinterpret_cast<const uint8_t*>(self->infoName);
        output = &self->nameInfo;
        outputLength = sizeof(self->nameInfo);
        break;
      case SMB2_FILE_POSITION_INFORMATION:
        memset(&self->positionInfo, 0, sizeof(self->positionInfo));
        self->positionInfo.current_byte_offset = handle->position;
        output = &self->positionInfo;
        outputLength = sizeof(self->positionInfo);
        break;
      case SMB2_FILE_STREAM_INFORMATION:
        if (!handle->directory) {
          memset(&self->streamInfo, 0, sizeof(self->streamInfo));
          self->streamInfo.stream_name = "::$DATA";
          self->streamInfo.stream_name_length = strlen("::$DATA");
          self->streamInfo.stream_size = size;
          self->streamInfo.stream_allocation_size =
              self->reportedAllocationSize(handle->physicalSize, false);
          output = &self->streamInfo;
          outputLength = sizeof(self->streamInfo);
        }
        break;
      default:
        break;
    }
  } else if (request->info_type == SMB2_0_INFO_FILESYSTEM &&
             request->file_info_class == SMB2_FILE_FS_ATTRIBUTE_INFORMATION) {
    // Стандартные атрибуты тома FAT32 в Windows:
    // FILE_CASE_PRESERVED_NAMES (0x2) | FILE_UNICODE_ON_DISK (0x4) = 0x06.
    // Флаг FILE_NAMED_STREAMS (0x40) отсутствует.
    memset(&self->attributeInfo, 0, sizeof(self->attributeInfo));
    self->attributeInfo.filesystem_attributes = 0x00000006;
    self->attributeInfo.maximum_component_name_length = 255;
    self->attributeInfo.filesystem_name =
        reinterpret_cast<const uint8_t*>("FAT32");
    self->attributeInfo.filesystem_name_length = strlen("FAT32") * 2;
    output = &self->attributeInfo;
    outputLength = sizeof(self->attributeInfo);
  } else if (request->info_type == SMB2_0_INFO_FILESYSTEM &&
             request->file_info_class == SMB2_FILE_FS_DEVICE_INFORMATION) {
    memset(&self->deviceInfo, 0, sizeof(self->deviceInfo));
    self->deviceInfo.device_type = FILE_DEVICE_DISK;
    self->deviceInfo.characteristics =
        FILE_REMOTE_DEVICE | FILE_DEVICE_IS_MOUNTED;
    output = &self->deviceInfo;
    outputLength = sizeof(self->deviceInfo);
  } else if (request->info_type == SMB2_0_INFO_FILESYSTEM &&
             request->file_info_class == SMB2_FILE_FS_CONTROL_INFORMATION) {
    memset(&self->controlInfo, 0, sizeof(self->controlInfo));
    output = &self->controlInfo;
    outputLength = sizeof(self->controlInfo);
  } else if (request->info_type == SMB2_0_INFO_FILESYSTEM) {
    // Проводник опрашивает свободное место в течение всего копирования.
    // Раньше каждый такой запрос закрывал активный файл и пересчитывал FAT,
    // из-за чего последовательная запись рвалась на каждом опросе.
    VfsFsInfo cachedInfo = {};
    uint8_t fsStatus = 0;
    if (!self->loadFsInfo(cachedInfo, fsStatus)) {
      return replyStatus(smb2, SMB2_QUERY_INFO,
                         fsStatus != 0 ? smbStatusFromFilex(fsStatus)
                                       : SMB2_STATUS_IO_DEVICE_ERROR);
    }
    const VfsFsInfo& fsInfo = cachedInfo;
    if (fsInfo.bytesPerSector == 0 || fsInfo.sectorsPerCluster == 0 ||
        fsInfo.totalClusters == 0 || (fsInfo.flags & 0x04) == 0 ||
        fsInfo.freeClusters > fsInfo.totalClusters) {
      return replyStatus(smb2, SMB2_QUERY_INFO,
                         SMB2_STATUS_IO_DEVICE_ERROR);
    }
    const uint64_t totalUnits = fsInfo.totalClusters;
    const uint64_t freeUnits = fsInfo.freeClusters;
    switch (request->file_info_class) {
      case SMB2_FILE_FS_VOLUME_INFORMATION:
        memset(&self->volumeInfo, 0, sizeof(self->volumeInfo));
        self->volumeInfo.volume_serial_number = fsInfo.serial;
        snprintf(self->volumeLabel, sizeof(self->volumeLabel), "%s",
                 (fsInfo.flags & 0x08) != 0 && fsInfo.label[0] != 0
                     ? fsInfo.label
                     : "ZX EVO SD");
        for (size_t length = strlen(self->volumeLabel);
             length != 0 && self->volumeLabel[length - 1] == ' '; --length) {
          self->volumeLabel[length - 1] = 0;
        }
        self->volumeInfo.volume_label =
            reinterpret_cast<const uint8_t*>(self->volumeLabel);
        self->volumeInfo.volume_label_length = strlen(self->volumeLabel) * 2;
        output = &self->volumeInfo;
        outputLength = sizeof(self->volumeInfo);
        break;
      case SMB2_FILE_FS_SIZE_INFORMATION:
        memset(&self->sizeInfo, 0, sizeof(self->sizeInfo));
        self->sizeInfo.total_allocation_units = totalUnits;
        self->sizeInfo.available_allocation_units = freeUnits;
        self->sizeInfo.sectors_per_allocation_unit =
            fsInfo.sectorsPerCluster;
        self->sizeInfo.bytes_per_sector = fsInfo.bytesPerSector;
        output = &self->sizeInfo;
        outputLength = sizeof(self->sizeInfo);
        break;
      case SMB2_FILE_FS_FULL_SIZE_INFORMATION:
        memset(&self->fullSizeInfo, 0, sizeof(self->fullSizeInfo));
        self->fullSizeInfo.total_allocation_units = totalUnits;
        self->fullSizeInfo.caller_available_allocation_units = freeUnits;
        self->fullSizeInfo.actual_available_allocation_units = freeUnits;
        self->fullSizeInfo.sectors_per_allocation_unit =
            fsInfo.sectorsPerCluster;
        self->fullSizeInfo.bytes_per_sector = fsInfo.bytesPerSector;
        output = &self->fullSizeInfo;
        outputLength = sizeof(self->fullSizeInfo);
        break;
      case SMB2_FILE_FS_SECTOR_SIZE_INFORMATION:
        memset(&self->sectorInfo, 0, sizeof(self->sectorInfo));
        self->sectorInfo.logical_bytes_per_sector = fsInfo.bytesPerSector;
        self->sectorInfo.physical_bytes_per_sector_for_atomicity =
            fsInfo.bytesPerSector;
        self->sectorInfo.physical_bytes_per_sector_for_performance =
            fsInfo.bytesPerSector;
        self->sectorInfo
            .file_system_effective_physical_bytes_per_sector_for_atomicity =
            fsInfo.bytesPerSector;
        output = &self->sectorInfo;
        outputLength = sizeof(self->sectorInfo);
        break;
      default:
        break;
    }
  } else if (request->info_type == SMB2_0_INFO_SECURITY) {
    // FAT/FILEX не предоставляет ACL. Нельзя отвечать фиктивным security
    // descriptor с DACL_PROTECTED и нулевым DACL: Windows принимает обычные
    // чтения, но отвергает такой ответ в CopyFile как Invalid Signature и
    // зависает до начала собственно копирования. Это тот же исправленный в
    // upstream ksmbd контракт: неподдерживаемые сведения возвращают ошибку.
    diagnosticLogEvent("SMB query-info security-not-supported add=%08lx path=%s",
                       static_cast<unsigned long>(
                           request->additional_information),
                       handle->path);
    return replyStatus(smb2, SMB2_QUERY_INFO, SMB2_STATUS_NOT_SUPPORTED);
  }

  if (output == nullptr || outputLength == 0) {
    diagnosticLogEvent("SMB query-info unsupported type=%u class=%u path=%s",
                       request->info_type, request->file_info_class,
                       handle->path);
    return replyStatus(smb2, SMB2_QUERY_INFO, SMB2_STATUS_NOT_SUPPORTED);
  }
  reply->output_buffer = output;
  reply->output_buffer_length = outputLength;
  self->sendOperation("INFO", handle->path);
  return 0;
}

int SmbServer::Impl::setInfoHandler(smb2_server* serverValue,
                                    smb2_context* smb2,
                                    smb2_set_info_request* request) {
  Impl* self = from(serverValue);
  int slot = -1;
  Handle* handle = self == nullptr || request == nullptr
                       ? nullptr
                       : self->findHandle(smb2, request->file_id, &slot);
  if (handle == nullptr || request->input_data == nullptr) {
    return replyStatus(smb2, SMB2_SET_INFO, SMB2_STATUS_INVALID_HANDLE);
  }
  if (request->info_type != SMB2_0_INFO_FILE) {
    return replyStatus(smb2, SMB2_SET_INFO, SMB2_STATUS_NOT_SUPPORTED);
  }
  const uint8_t* data = static_cast<const uint8_t*>(request->input_data);

  switch (request->file_info_class) {
    case SMB2_FILE_BASIC_INFORMATION:
      if (request->buffer_length < 40) {
        return replyStatus(smb2, SMB2_SET_INFO,
                           SMB2_STATUS_INVALID_PARAMETER);
      }
      {
        VfsMetadata metadata;
        const uint32_t attributes = readLe32(data + 32);
        if (attributes != 0) {
          metadata.attrMask = 0x27;
          metadata.attrValue = static_cast<uint8_t>(attributes & 0x27);
        }
        uint16_t fatDate = 0;
        uint16_t fatTime = 0;
        metadata.createFileTime = readLe64Local(data);
        metadata.accessFileTime = readLe64Local(data + 8);
        metadata.writeFileTime = readLe64Local(data + 16);
        if (fileTimeToFat(metadata.createFileTime, fatDate, fatTime,
                          &metadata.createTenth)) {
          metadata.timeMask |= 0x01;
          metadata.createDate = fatDate;
          metadata.createTime = fatTime;
        }
        if (fileTimeToFat(metadata.accessFileTime, fatDate, fatTime)) {
          metadata.timeMask |= 0x02;
          metadata.accessDate = fatDate;
        }
        if (fileTimeToFat(metadata.writeFileTime, fatDate, fatTime)) {
          metadata.timeMask |= 0x04;
          metadata.writeDate = fatDate;
          metadata.writeTime = fatTime;
        }
        if (metadata.attrMask == 0 && metadata.timeMask == 0) {
          return 0;
        }
        if (self->hasAsyncWritesForHandle(slot, handle->generation)) {
          self->deferMetadata(*handle, metadata);
          self->sendOperation("ATTR CACHE", handle->path);
          self->notifyChange(
              handle->path, SMB2_NOTIFY_CHANGE_FILE_ACTION_MODIFIED,
              SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_ATTRIBUTES |
                  SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_CREATION |
                  SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_LAST_ACCESS |
                  SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_LAST_WRITE);
          return 0;
        }
        self->deferMetadata(*handle, metadata);
        self->sendOperation("ATTR", handle->path);
        self->notifyChange(
            handle->path, SMB2_NOTIFY_CHANGE_FILE_ACTION_MODIFIED,
            SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_ATTRIBUTES |
                SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_CREATION |
                SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_LAST_ACCESS |
                SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_LAST_WRITE);
        return 0;
      }

    case SMB2_FILE_POSITION_INFORMATION:
      if (request->buffer_length < 8 || readLe64Local(data) > UINT32_MAX) {
        return replyStatus(smb2, SMB2_SET_INFO,
                           SMB2_STATUS_INVALID_PARAMETER);
      }
      handle->position = static_cast<uint32_t>(readLe64Local(data));
      return 0;

    case SMB2_FILE_DISPOSITION_INFORMATION:
        // Копия файла в PSRAM после правки недостоверна.
        self->dropFileCache();
      if (request->buffer_length < 1 || strcmp(handle->path, "/") == 0) {
        return replyStatus(smb2, SMB2_SET_INFO,
                           SMB2_STATUS_INVALID_PARAMETER);
      }
      if (data[0] != 0 && handle->directory) {
        const DirectoryContents contents =
            self->checkDirectoryContents(handle->path);
        if (contents == DirectoryContents::kNotEmpty) {
          return replyStatus(smb2, SMB2_SET_INFO,
                             SMB2_STATUS_DIRECTORY_NOT_EMPTY);
        }
        if (contents == DirectoryContents::kIoError) {
          return replyStatus(smb2, SMB2_SET_INFO,
                             SMB2_STATUS_IO_DEVICE_ERROR);
        }
      }
      handle->deletePending = data[0] != 0;
      self->sendOperation(handle->deletePending ? "DELETE?" : "KEEP",
                          handle->path);
      return 0;

    case SMB2_FILE_ALLOCATION_INFORMATION: {
      if (!handle->writable || handle->directory ||
          request->buffer_length < 8 || readLe64Local(data) > UINT32_MAX) {
        return replyStatus(smb2, SMB2_SET_INFO,
                           SMB2_STATUS_INVALID_PARAMETER);
      }
      // AllocationSize — подсказка о предварительном резервировании, а не EOF.
      // FAT/WC не умеет резервировать кластеры без изменения длины, поэтому
      // принимаем подсказку и оставляем последовательную запись неизменной.
      self->sendOperation("ALLOCATE", handle->path);
      return 0;
    }

    case SMB2_FILE_END_OF_FILE_INFORMATION: {
        // Копия файла в PSRAM после правки недостоверна.
        self->dropFileCache();
      if (!handle->writable || handle->directory ||
          request->buffer_length < 8 || readLe64Local(data) > UINT32_MAX) {
        return replyStatus(smb2, SMB2_SET_INFO,
                           SMB2_STATUS_INVALID_PARAMETER);
      }
      const uint32_t requestedSize =
          static_cast<uint32_t>(readLe64Local(data));

      // Проводник задаёт конечный EOF до первого WRITE. Немедленное физическое
      // расширение пустого FAT-файла нулями занимало столько же времени, сколько
      // последующая запись всего файла, поэтому индикатор копирования долго
      // оставался на нуле. Сначала заканчиваем уже принятые WRITE, после чего
      // рост сохраняем как логический размер. Обычные WRITE материализуют его
      // последовательно; недостающий хвост расширяется только на CLOSE/FLUSH.
      if (!self->drainAsyncWritesForHandle(slot, handle->generation)) {
        handle->failed = true;
        return replyStatus(smb2, SMB2_SET_INFO,
                           SMB2_STATUS_IO_DEVICE_ERROR);
      }
      if (requestedSize > handle->physicalSize) {
        handle->sizeReserved = true;
        handle->ownsSizeReservation = true;
        handle->reservedSize = requestedSize;
        self->sendOperation("RESERVE", handle->path);
        self->notifyChange(
            handle->path, SMB2_NOTIFY_CHANGE_FILE_ACTION_MODIFIED,
            SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_SIZE |
                SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_LAST_WRITE);
        return 0;
      }
      if (requestedSize == handle->physicalSize) {
        handle->sizeReserved = false;
        handle->ownsSizeReservation = false;
        handle->reservedSize = requestedSize;
        return 0;
      }
      if (!self->activateWrite(slot, handle->position)) {
        return replyStatus(smb2, SMB2_SET_INFO,
                           SMB2_STATUS_IO_DEVICE_ERROR);
      }
      VfsResult result = {};
      if (!self->requestVfs(VfsOperation::kSetEof, nullptr, requestedSize,
                            result, kMutateVfsTimeoutMs) ||
          result.size != requestedSize) {
        return replyStatus(
            smb2, SMB2_SET_INFO,
            result.status != 0 ? smbStatusFromFilex(result.status)
                               : SMB2_STATUS_IO_DEVICE_ERROR);
      }
      self->invalidateFsInfo();
      self->updateSharedPhysicalSize(handle->path, requestedSize);
      for (size_t index = 0; index < kHandleCount; ++index) {
        Handle& shared = self->handles[index];
        if (shared.used && asciiEqualNoCase(shared.path, handle->path)) {
          shared.reservedSize = requestedSize;
          shared.sizeReserved = false;
          shared.ownsSizeReservation = false;
        }
      }
      if (handle->position > requestedSize) {
        handle->position = requestedSize;
      }
      handle->metadataDirty = true;
      self->activeLogicalOffset = handle->position;
      self->activeVfsOffset = handle->position;
      self->sendOperation(requestedSize == 0 ? "TRUNCATE" : "SETEOF",
                          handle->path);
      self->notifyChange(
          handle->path, SMB2_NOTIFY_CHANGE_FILE_ACTION_MODIFIED,
          SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_SIZE |
              SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_LAST_WRITE);
      return 0;
    }

    case SMB2_FILE_RENAME_INFORMATION: {
        // Копия файла в PSRAM после правки недостоверна.
        self->dropFileCache();
      if (!handle->writable || strcmp(handle->path, "/") == 0 ||
          request->buffer_length < 20) {
        return replyStatus(smb2, SMB2_SET_INFO,
                           SMB2_STATUS_INVALID_PARAMETER);
      }
      const uint32_t byteLength = readLe32(data + 16);
      if ((byteLength & 1U) != 0 || byteLength == 0 ||
          byteLength > request->buffer_length - 20) {
        return replyStatus(smb2, SMB2_SET_INFO,
                           SMB2_STATUS_INVALID_PARAMETER);
      }
      const char* converted = smb2_utf16_to_utf8(
          reinterpret_cast<const uint16_t*>(data + 20), byteLength / 2);
      if (converted == nullptr) {
        return replyStatus(smb2, SMB2_SET_INFO,
                           SMB2_STATUS_OBJECT_NAME_INVALID);
      }
      char target[kMaxPath + 1];
      const bool normalized = self->normalizePath(converted, target);
      free(const_cast<char*>(converted));
      if (!normalized) {
        return replyStatus(smb2, SMB2_SET_INFO,
                           SMB2_STATUS_OBJECT_NAME_INVALID);
      }

      if (asciiEqualNoCase(handle->path, target)) {
        return 0;
      }
      if (!self->closeActive(true)) {
        return replyStatus(smb2, SMB2_SET_INFO,
                           SMB2_STATUS_IO_DEVICE_ERROR);
      }

      const bool replace = data[0] != 0;
      VfsResult result = {};
      char oldPath[kMaxPath + 1];
      snprintf(oldPath, sizeof(oldPath), "%s", handle->path);

      char oldParent[kMaxPath + 1] = {};
      char oldName[kMaxPath + 1] = {};
      char targetParent[kMaxPath + 1] = {};
      char targetName[kMaxPath + 1] = {};
      const bool sameDir = self->splitParent(oldPath, oldParent, oldName) &&
                           self->splitParent(target, targetParent, targetName) &&
                           asciiEqualNoCase(oldParent, targetParent);

      bool ok = false;
      // Legacy RENAME has no ReplaceIfExists bit.  For replacement, pass the
      // original SMB intent to FILEX MOVE_RENAME even inside one directory;
      // pre-deleting the destination made the operation non-atomic and hid a
      // failed delete behind a later, misleading rename error.
      if (sameDir && !replace) {
        ok = self->requestRename(oldPath, targetName, handle->directory, result);
      }
      if (!ok) {
        ok = self->requestMoveRename(oldPath, target, handle->directory,
                                     replace, result);
      }
      if (!ok) {
        return replyStatus(
            smb2, SMB2_SET_INFO,
            result.status != 0 ? smbStatusFromFilex(result.status)
                               : SMB2_STATUS_IO_DEVICE_ERROR);
      }
      self->invalidateParent(oldPath);
      self->invalidateParent(target);
      self->invalidateSubtree(oldPath);
      self->invalidateSubtree(target);
      self->renameMetadata(oldPath, target);
      snprintf(handle->path, sizeof(handle->path), "%s", target);
      self->sendOperation("RENAME", target);
      self->notifyRename(
          oldPath, target,
          handle->directory ? SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_DIR_NAME
                            : SMB2_CHANGE_NOTIFY_FILE_NOTIFY_CHANGE_FILE_NAME);
      return 0;
    }

    default:
      return replyStatus(smb2, SMB2_SET_INFO, SMB2_STATUS_NOT_SUPPORTED);
  }
}

SmbServer::SmbServer(VfsBridge& bridge, EventSink eventSink,
                     void* eventContext)
    : impl_(new (std::nothrow) Impl(bridge, eventSink, eventContext)) {}

SmbServer::~SmbServer() {
  if (impl_ == nullptr) {
    return;
  }
  // Объект глобальный и в обычной прошивке не уничтожается. Проверка всё же
  // делает класс безопасным для тестов и будущего динамического использования.
  if (impl_->stop()) {
    delete impl_;
  }
  impl_ = nullptr;
}

bool SmbServer::start(const uint8_t* payload, uint16_t length,
                      uint16_t& actualPort, bool& netbiosActive,
                      char* error, size_t errorSize) {
  if (impl_ == nullptr) {
    setExternalError(error, errorSize, "smb no memory");
    actualPort = 0;
    netbiosActive = false;
    return false;
  }
  return impl_->start(payload, length, actualPort, netbiosActive, error,
                      errorSize);
}

bool SmbServer::stop() {
  return impl_ == nullptr || impl_->stop();
}

void SmbServer::pollDiscovery() {
  if (impl_ != nullptr) {
    impl_->pollDiscovery();
  }
}

bool SmbServer::running() const {
  return impl_ != nullptr && impl_->runningFlag;
}

uint16_t SmbServer::port() const {
  return impl_ == nullptr ? 0 : impl_->portValue;
}

bool SmbServer::netbiosActive() const {
  return impl_ != nullptr && impl_->discoveryRunning;
}

}  // namespace zifi
