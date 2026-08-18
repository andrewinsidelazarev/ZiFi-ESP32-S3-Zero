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

// Важно соблюдать этот порядок: libsmb2.h использует типы, объявленные в
// smb2.h. Заголовки написаны на C, поэтому окружаем их C-связыванием.
extern "C" {
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/libsmb2-dcerpc-server.h>
#include <smb2/smb2-errors.h>
#include <smb2/smb2-ioctl.h>
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
constexpr uint32_t kSmbAdvertisedIoSize = 64 * 1024;
// READ и WRITE_THROUGH удерживают свой SMB-кредит до физического завершения,
// поэтому watchdog должен сработать раньше обычного 120-секундного PDU timeout.
constexpr uint32_t kAsyncIoProgressTimeoutMs = 90 * 1000;
// Windows CopyFile держит несколько READ/WRITE одновременно и вправе присылать
// их не по порядку файловых смещений. Каждый запрос получает собственный слот в
// PSRAM, после чего core 1 обрабатывает последовательные offsets. Один общий
// пул не конфликтует с отдельной областью кэша каталога и не удваивает память.
// 12 слотов покрывают наблюдавшееся окно из десяти запросов с запасом.
constexpr size_t kAsyncIoQueueDepth = 12;
constexpr size_t kAsyncIoSlotSize = kSmbAdvertisedIoSize;
// NEGOTIATE выдаёт Windows три кредита. Обычные WRITE подтверждаются после
// сохранения полного payload в PSRAM, но последние три места в очереди служат
// обратным давлением: их ответы задерживаются, пока write-back не освободит
// место для запросов, которые клиент сможет прислать на возвращённые кредиты.
constexpr size_t kAsyncWriteEarlyReplyDepth = kAsyncIoQueueDepth - 3;
// Самая короткая поддерживаемая запись: FILE_FULL_DIRECTORY_INFORMATION,
// односимвольное имя UTF-16 и обязательное 8-байтное выравнивание. Этого числа
// достаточно,
// чтобы заранее выделить в PSRAM массив структур для любого ответа 64 КиБ.
constexpr size_t kMinimumDirectoryEntrySize = 72;
constexpr size_t kDirectoryBatchCapacity =
    kSmbAdvertisedIoSize / kMinimumDirectoryEntrySize;
constexpr uint32_t kFileIdMagic = 0x424D535AUL;  // "ZSMB" в little-endian.

constexpr uint32_t kCreateSuperseded = 1;
constexpr uint32_t kCreateOpened = 2;
constexpr uint32_t kCreateCreated = 3;
constexpr uint32_t kCreateOverwritten = 4;

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

uint64_t allocationSize(uint64_t size) {
  return (size + 511U) & ~static_cast<uint64_t>(511U);
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

  struct Handle {
    bool used = false;
    bool directory = false;
    bool pipe = false;
    bool writable = false;
    bool metadataDirty = false;
    bool metadataPending = false;
    bool deletePending = false;
    bool createdNew = false;
    bool failed = false;
    bool sizeReserved = false;
    uint32_t generation = 0;
    uint32_t physicalSize = 0;
    // Длина файла на момент открытия. Разницу с итоговой сообщаем владельцу
    // счётчика свободного места один раз, на CLOSE: делать это на каждом
    // блоке нельзя — синхронный обмен с мостом посреди приёма данных
    // блокирует SMB-задачу и Windows теряет сессию.
    uint32_t openedSize = 0;
    uint32_t reservedSize = 0;
    uint32_t position = 0;
    uint32_t directoryIndex = 0;
    bool directoryEnded = false;
    bool directoryCacheUnavailable = false;
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

  struct Tree {
    bool used = false;
    bool ipc = false;
    uint32_t id = 0;
  };

  // Payload уже скопирован в соответствующий фиксированный PSRAM-слот.
  // Метаданные остаются до финального ответа на исходный async SMB-запрос.
  struct AsyncWrite {
    bool used = false;
    bool inFlight = false;
    bool cancelRequested = false;
    bool replied = false;
    bool writeThrough = false;
    smb2_context* context = nullptr;
    uint64_t messageId = 0;
    int slot = -1;
    uint32_t generation = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t flushed = 0;
    uint32_t windowLength = 0;
    uint32_t lastProgressMs = 0;
  };

  // READ накапливает до 64 КиБ из нескольких физических VFS-окон. inFlight
  // означает, что одно окно прямо сейчас принадлежит core 1.
  struct AsyncRead {
    bool used = false;
    bool inFlight = false;
    bool cancelRequested = false;
    smb2_context* context = nullptr;
    uint64_t messageId = 0;
    int slot = -1;
    uint32_t generation = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t filled = 0;
    uint32_t windowLength = 0;
    uint32_t lastProgressMs = 0;
  };

  Impl(VfsBridge& bridgeValue, EventSink sink, void* sinkContext)
      : bridge(bridgeValue),
        eventSink(sink),
        eventContext(sinkContext),
        udp(),
        wsDiscovery(),
        server{},
        handlers{},
        task(nullptr),
        taskAlive(false),
        runningFlag(false),
        discoveryRunning(false),
        lastServeResult(0),
        client(nullptr),
        trees{},
        nextTreeId(kFirstTreeId),
        handles{},
        generationCounter(1),
        lastCreatedSlot(-1),
        activeSlot(-1),
        activeMode(ActiveMode::kNone),
        activeLogicalOffset(0),
        activeVfsOffset(0),
        asyncReads{},
        asyncWrites{},
        asyncIoBuffers{},
        activeAsyncRead(-1),
        activeAsyncWrite(-1),
        asyncReadCount(0),
        asyncWriteCount(0),
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
        cachedFsInfo{},
        fsInfoValid(false),
        volumeInfo{},
        sizeInfo{},
        fullSizeInfo{},
        attributeInfo{},
        deviceInfo{},
        controlInfo{},
        sectorInfo{},
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
    releaseDirectoryBatch();
    releaseAsyncIoStorage();
  }

  VfsBridge& bridge;
  EventSink eventSink;
  void* eventContext;
  WiFiUDP udp;
  WsDiscovery wsDiscovery;
  smb2_server server;
  smb2_server_request_handlers handlers;
  TaskHandle_t task;
  volatile bool taskAlive;
  volatile bool runningFlag;
  bool discoveryRunning;
  volatile int lastServeResult;
  smb2_context* client;

  Tree trees[kTreeCount];
  uint32_t nextTreeId;
  Handle handles[kHandleCount];
  uint32_t generationCounter;
  int lastCreatedSlot;
  int activeSlot;
  ActiveMode activeMode;
  uint32_t activeLogicalOffset;
  uint32_t activeVfsOffset;
  AsyncRead asyncReads[kAsyncIoQueueDepth];
  AsyncWrite asyncWrites[kAsyncIoQueueDepth];
  uint8_t* asyncIoBuffers[kAsyncIoQueueDepth];
  int activeAsyncRead;
  int activeAsyncWrite;
  size_t asyncReadCount;
  size_t asyncWriteCount;

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
  // Сведения о томе. Кэш локальный и намеренно: любой обмен с мостом внутри
  // приёма данных блокирует SMB-задачу, и Windows теряет сессию.
  VfsFsInfo cachedFsInfo;
  bool fsInfoValid;
  smb2_file_fs_volume_info volumeInfo;
  smb2_file_fs_size_info sizeInfo;
  smb2_file_fs_full_size_info fullSizeInfo;
  smb2_file_fs_attribute_info attributeInfo;
  smb2_file_fs_device_info deviceInfo;
  smb2_file_fs_control_info controlInfo;
  smb2_file_fs_sector_size_info sectorInfo;

  uint8_t scratch[512];
  uint8_t nbnsPacket[576];

  bool start(const uint8_t* payload, uint16_t length, uint16_t& actualPort,
             bool& netbiosActive, char* error, size_t errorSize);
  bool stop();
  void pollDiscovery();
  void startDiscovery();
  void stopDiscovery();
  void answerNbns(size_t length);

  static void taskEntry(void* context);
  void taskLoop();
  static void newClient(smb2_context* smb2, void* context);
  static void libraryError(smb2_context* smb2, const char* text);

  void resetHandles();
  bool allocateAsyncIoStorage();
  void releaseAsyncIoStorage();
  void resetAsyncIo();
  int allocateAsyncRead();
  int allocateAsyncWrite();
  int findReadyAsyncRead() const;
  int findReadyAsyncWrite() const;
  bool hasAsyncWritesForHandle(int slot, uint32_t generation) const;
  bool drainAsyncWritesForHandle(int slot, uint32_t generation);
  bool asyncIoTimedOut() const;
  void failAsyncReads(uint32_t status);
  void failAsyncWrites(uint32_t status);
  void releaseClientHandles();
  bool allocateDirectoryBatch();
  void releaseDirectoryBatch();
  bool clientBusyForReplacement(smb2_context* smb2) const;
  void cleanupClient(smb2_context* smb2);
  void sendClientEvent(uint8_t state);
  void sendOperation(const char* operation, const char* path = nullptr);
  // Ход текущей передачи. force обязателен на завершении, иначе последнее
  // обновление может не пройти ограничение частоты и счётчик замрёт до конца.
  void sendProgress(const Handle& handle, const char* operation, bool force);
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
  void invalidateDirectory(const char* path);
  void invalidateSubtree(const char* path);
  void invalidateParent(const char* path);
  // Сведения о томе читаются через кэш: это единственная операция, которая
  // может заставить Wild Commander пересчитать всю FAT, а Проводник опрашивает
  // свободное место в течение всего копирования.
  bool loadFsInfo(VfsFsInfo& info, uint8_t& status);
  void invalidateFsInfo();
  // Чистая арифметика над локальным кэшем: ни UART, ни моста.
  void noteFileGrowth(uint32_t oldSize, uint32_t newSize);
  // Обновляет размер записи в снимке родителя без сброса всего снимка. Именно
  // отсюда Проводник берёт размер растущего файла во время копирования.
  void refreshCachedSize(const char* path, uint32_t size);
  bool activateRead(int slot, uint32_t offset);
  bool fetchReadWindow(Handle& handle);
  bool activateWrite(int slot, uint32_t offset);
  bool commitReservedSize(int slot);
  void deferMetadata(Handle& handle, const VfsMetadata& metadata);
  bool applyPendingMetadata(int slot);
  bool activateDirectory(int slot);
  void pollAsyncRead();
  void pollAsyncWrite();
  bool queueBufferedWriteReplies();
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

  int allocateHandle();
  Handle* findHandle(const uint8_t fileId[SMB2_FD_SIZE], int* slot = nullptr);
  void releaseHandle(int slot);
  uint32_t allocateTree(bool ipc);
  const Tree* findTree(uint32_t id) const;
  void releaseTree(uint32_t id);
  uint32_t visibleSize(const Handle& handle) const;
  uint64_t directoryFileId(const char* name) const;
  void fillDirectoryInfo(smb2_fileidbothdirectoryinformation& info,
                         uint32_t index, bool directory, uint32_t size,
                         const char* name) const;
  int queryCachedDirectory(smb2_context* smb2, Handle& handle,
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
  memset(&server, 0, sizeof(server));
  server.fd = -1;
  server.port = portValue;
  server.handlers = &handlers;
  server.auth_data = this;
  server.signing_enabled = 1;
  server.allow_anonymous = 0;
  server.max_transact_size = kSmbAdvertisedIoSize;
  server.max_read_size = kSmbAdvertisedIoSize;
  server.max_write_size = kSmbAdvertisedIoSize;
  snprintf(server.hostname, sizeof(server.hostname), "%s", hostname);
  snprintf(server.domain, sizeof(server.domain), "%s", workgroup);

  // GUID должен быть устойчивым для конкретного адаптера. Первые восемь байт
  // читаем как подпись, последние восемь — из заводского MAC ESP32-S3.
  memcpy(server.guid, "ZiFiSMB!", 8);
  const uint64_t mac = ESP.getEfuseMac();
  for (size_t index = 0; index < 8; ++index) {
    server.guid[8 + index] = static_cast<uint8_t>(mac >> (index * 8));
  }

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
    while ((activeAsyncRead >= 0 || activeAsyncWrite >= 0) &&
           static_cast<uint32_t>(millis() - drainStarted) <
               kMutateVfsTimeoutMs) {
      pollAsyncRead();
      pollAsyncWrite();
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
  if (client != nullptr) {
    cleanupClient(client);
  }
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
  // VFS физически односессионный, но Windows вправе открыть новый TCP-сеанс
  // к тому же серверу (например, после обращения сначала по IP, затем по
  // имени). Завершённый старый сеанс передаёт владение новому. Незавершённый
  // файловый обмен и ещё не отправленный SMB-ответ прерывать нельзя.
  if (self->client != nullptr && self->client != smb2) {
    smb2_context* previous = self->client;
    if (self->clientBusyForReplacement(previous)) {
      diagnosticLogEvent(
          "SMB client-reject-busy new=%p active=%p mode=%u slot=%d", smb2,
          previous, static_cast<unsigned>(self->activeMode), self->activeSlot);
      smb2_close_context(smb2);
      return;
    }
    diagnosticLogEvent("SMB client-replace previous=%p new=%p mode=%u",
                       previous, smb2,
                       static_cast<unsigned>(self->activeMode));
    self->cleanupClient(previous);
    smb2_close_context(previous);
  }
  self->client = smb2;
  diagnosticLogEvent("SMB client-accepted client=%p", smb2);
  self->resetAsyncIo();
  self->resetHandles();
  // В серверной части libsmb2 6.1.0 расчёт pre-auth hash для SMB 3.1.1
  // пока несовместим с Windows: последний SESSION_SETUP получается с неверной
  // подписью, и Проводник показывает безликую ошибку 0x80004005. SMB 2.1,
  // 3.0 и 3.0.2 проверены теми же zx/zx и работают. Поэтому до отдельного
  // исправления 3.1.1 объявляем максимальным современный диалект SMB 3.0.2.
  smb2_set_version(smb2, SMB2_VERSION_0302);
  // Windows проверяет, что ответ FSCTL_VALIDATE_NEGOTIATE_INFO подписан, даже
  // когда в NEGOTIATE клиент только разрешил подпись, но не потребовал её.
  // Серверный код libsmb2 6.1.0 иначе оставляет этот ответ неподписанным, и
  // Проводник завершает уже успешно авторизованный сеанс с ошибкой 1208.
  // Обязательная подпись через штатный API библиотеки одновременно исправляет
  // проверку диалекта и защищает все последующие запросы данного клиента.
  smb2_set_sign(smb2, 1);
  smb2_set_authentication(smb2, SMB2_SEC_NTLMSSP);
  smb2_set_timeout(smb2, 120);
  smb2_register_error_callback(smb2, libraryError);
  self->sendClientEvent(1);
}

void SmbServer::Impl::libraryError(smb2_context*, const char* message) {
  // UART занят двоичным протоколом, поэтому библиотечные сообщения нельзя
  // печатать в Serial. Во временной диагностической сборке ошибка сохраняется
  // во flash и не портит двоичный UART-протокол.
  diagnosticLogEvent("SMB library-error %s",
                     message == nullptr ? "(null)" : message);
}

void SmbServer::Impl::resetHandles() {
  memset(trees, 0, sizeof(trees));
  memset(handles, 0, sizeof(handles));
  lastCreatedSlot = -1;
  activeSlot = -1;
  activeMode = ActiveMode::kNone;
  activeLogicalOffset = 0;
  activeVfsOffset = 0;
}

bool SmbServer::Impl::allocateAsyncIoStorage() {
  releaseAsyncIoStorage();
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    asyncIoBuffers[index] = static_cast<uint8_t*>(heap_caps_malloc(
        kAsyncIoSlotSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (asyncIoBuffers[index] == nullptr) {
      releaseAsyncIoStorage();
      return false;
    }
  }
  resetAsyncIo();
  return true;
}

void SmbServer::Impl::releaseAsyncIoStorage() {
  resetAsyncIo();
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    heap_caps_free(asyncIoBuffers[index]);
    asyncIoBuffers[index] = nullptr;
  }
}

void SmbServer::Impl::resetAsyncIo() {
  memset(asyncReads, 0, sizeof(asyncReads));
  memset(asyncWrites, 0, sizeof(asyncWrites));
  activeAsyncRead = -1;
  activeAsyncWrite = -1;
  asyncReadCount = 0;
  asyncWriteCount = 0;
}

int SmbServer::Impl::allocateAsyncRead() {
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

int SmbServer::Impl::allocateAsyncWrite() {
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

int SmbServer::Impl::findReadyAsyncRead() const {
  if (activeAsyncRead >= 0 || activeAsyncWrite >= 0 ||
      asyncWriteCount != 0) {
    return -1;
  }

  // Сначала продолжаем уже открытый поток, затем логическую позицию handle.
  // Последний проход нужен для корректного произвольного чтения (seek).
  for (int pass = 0; pass < 3; ++pass) {
    int fallback = -1;
    uint32_t fallbackOffset = UINT32_MAX;
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
      const bool continuesActive = activeMode == ActiveMode::kRead &&
                                   activeSlot == pending.slot &&
                                   activeLogicalOffset == pending.offset;
      const bool continuesHandle = handle.position == pending.offset;
      if ((pass == 0 && continuesActive) ||
          (pass == 1 && continuesHandle)) {
        return static_cast<int>(index);
      }
      if (pass == 2 && pending.offset < fallbackOffset) {
        fallback = static_cast<int>(index);
        fallbackOffset = pending.offset;
      }
    }
    if (pass == 2) {
      return fallback;
    }
  }
  return -1;
}

int SmbServer::Impl::findReadyAsyncWrite() const {
  if (activeAsyncWrite >= 0 || activeAsyncRead >= 0 ||
      asyncReadCount != 0) {
    return -1;
  }
  // FILEX допускает произвольные offsets. Сначала продолжаем текущую позицию,
  // затем берём наименьшее ожидающее смещение для стабильного порядка.
  for (int pass = 0; pass < 2; ++pass) {
    int fallback = -1;
    uint32_t fallbackOffset = UINT32_MAX;
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
      const uint32_t nextOffset = pending.offset + pending.flushed;
      const bool continuesActive =
          activeMode == ActiveMode::kWrite && activeSlot == pending.slot &&
          activeLogicalOffset == nextOffset;
      if (pass == 0 && continuesActive) {
        return static_cast<int>(index);
      }
      if (pass == 1 && nextOffset < fallbackOffset) {
        fallback = static_cast<int>(index);
        fallbackOffset = nextOffset;
      }
    }
    if (pass == 1) {
      return fallback;
    }
  }
  return -1;
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

bool SmbServer::Impl::drainAsyncWritesForHandle(int slot,
                                                uint32_t generation) {
  const uint32_t started = millis();
  for (;;) {
    if (!hasAsyncWritesForHandle(slot, generation)) {
      return slot >= 0 && static_cast<size_t>(slot) < kHandleCount &&
             handles[slot].used && handles[slot].generation == generation &&
             !handles[slot].failed;
    }
    pollAsyncWrite();
    if (static_cast<uint32_t>(millis() - started) >=
        kAsyncIoProgressTimeoutMs) {
      failAsyncWrites(SMB2_STATUS_IO_TIMEOUT);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

bool SmbServer::Impl::asyncIoTimedOut() const {
  const uint32_t now = millis();
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    if (asyncReads[index].used &&
        static_cast<uint32_t>(now - asyncReads[index].lastProgressMs) >=
            kAsyncIoProgressTimeoutMs) {
      return true;
    }
    if (asyncWrites[index].used &&
        static_cast<uint32_t>(now - asyncWrites[index].lastProgressMs) >=
            kAsyncIoProgressTimeoutMs) {
      return true;
    }
  }
  return false;
}

void SmbServer::Impl::failAsyncReads(uint32_t status) {
  smb2_context* contextToClose = nullptr;
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    AsyncRead& pending = asyncReads[index];
    if (!pending.used) {
      continue;
    }
    if (pending.context != nullptr &&
        !queueAsyncStatus(pending.context, SMB2_READ, status,
                          pending.messageId)) {
      contextToClose = pending.context;
    }
    pending = {};
  }
  activeAsyncRead = -1;
  asyncReadCount = 0;
  closeActive(false);
  if (contextToClose != nullptr) {
    smb2_close_context(contextToClose);
  }
}

void SmbServer::Impl::failAsyncWrites(uint32_t status) {
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
    if (pending.context != nullptr && !pending.replied &&
        !queueAsyncStatus(pending.context, SMB2_WRITE, status,
                          pending.messageId)) {
      contextToClose = pending.context;
    }
    pending = {};
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
  return directoryBatch != nullptr;
}

void SmbServer::Impl::releaseDirectoryBatch() {
  heap_caps_free(directoryBatch);
  directoryBatch = nullptr;
}

bool SmbServer::Impl::clientBusyForReplacement(smb2_context* smb2) const {
  // Закрытие контекста с ответом в outqueue способно оборвать уже выполненную
  // операцию ровно перед передачей данных клиенту.
  if (smb2 != nullptr && (smb2_which_events(smb2) & POLLOUT) != 0) {
    return true;
  }
  if (asyncReadCount != 0 || asyncWriteCount != 0) {
    return true;
  }
  if (activeMode == ActiveMode::kWrite) {
    return true;
  }
  if (activeMode != ActiveMode::kRead) {
    return false;
  }
  if (activeSlot < 0 || static_cast<size_t>(activeSlot) >= kHandleCount) {
    return true;
  }
  const Handle& handle = handles[activeSlot];
  return !handle.used || handle.position < handle.physicalSize;
}

uint32_t SmbServer::Impl::allocateTree(bool ipc) {
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
  if (smb2 != client) {
    diagnosticLogEvent("SMB cleanup-nonowner client=%p active=%p", smb2,
                       client);
    return;
  }
  diagnosticLogEvent("SMB cleanup-owner client=%p", smb2);
  bool hadAsyncIo = false;
  bool ioInFlight = false;
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    AsyncRead& pending = asyncReads[index];
    if (!pending.used || pending.context != smb2) {
      continue;
    }
    hadAsyncIo = true;
    ioInFlight = ioInFlight || pending.inFlight;
    pending.context = nullptr;
    pending.cancelRequested = true;
  }
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    AsyncWrite& pending = asyncWrites[index];
    if (!pending.used || pending.context != smb2) {
      continue;
    }
    hadAsyncIo = true;
    // Все уже принятые payload находятся в независимых PSRAM-слотах. После
    // разрыва соединения дописываем их на SD даже без возможности отправить
    // SMB-ответ: ранее подтверждённый write-back нельзя молча потерять.
    ioInFlight = true;
    pending.context = nullptr;
    pending.replied = true;
    pending.cancelRequested = false;
  }
  if (hadAsyncIo && ioInFlight) {
    // Core 1 ещё владеет UART и одним из колец. Окончательную очистку выполнит
    // serviceHandler после возврата текущего VFS-окна.
    client = nullptr;
    sendClientEvent(0);
    diagnosticLogEvent("SMB cleanup-deferred async-io");
    return;
  }
  if (hadAsyncIo) {
    closeActive(false);
    resetAsyncIo();
  }
  closeActive(true);
  releaseClientHandles();
  client = nullptr;
  // Следующий клиент начинает с чистого поля индикации, а дедупликация не
  // должна проглотить его первое событие из-за совпадения с прошлым сеансом.
  lastOperationText[0] = 0;
  resetProgress();
  sendClientEvent(0);
  diagnosticLogEvent("SMB cleanup-done");
}

void SmbServer::Impl::releaseClientHandles() {
  for (size_t index = 0; index < kHandleCount; ++index) {
    if (handles[index].used && handles[index].deletePending &&
        strcmp(handles[index].path, "/") != 0) {
      removePath(handles[index].path);
    } else if (handles[index].used && handles[index].metadataDirty) {
      invalidateParent(handles[index].path);
    }
  }
  if (activeAsyncRead < 0 && activeAsyncWrite < 0) {
    resetAsyncIo();
  }
  resetHandles();
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

// Человекочитаемый размер с одним знаком после запятой. Делить и печатать на
// стороне ESP дешевле, чем городить 32-битную арифметику в плагине Z80.
static void formatTransferSize(uint32_t bytes, char* out, size_t capacity) {
  const uint32_t kMega = 1024UL * 1024UL;
  const uint32_t kKilo = 1024UL;
  if (bytes >= kMega) {
    snprintf(out, capacity, "%lu.%luM",
             static_cast<unsigned long>(bytes / kMega),
             static_cast<unsigned long>(((bytes % kMega) * 10UL) / kMega));
  } else if (bytes >= kKilo) {
    snprintf(out, capacity, "%lu.%luK",
             static_cast<unsigned long>(bytes / kKilo),
             static_cast<unsigned long>(((bytes % kKilo) * 10UL) / kKilo));
  } else {
    snprintf(out, capacity, "%luB", static_cast<unsigned long>(bytes));
  }
}

void SmbServer::Impl::sendProgress(const Handle& handle, const char* operation,
                                   bool force) {
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
  formatTransferSize(handle.position, doneText, sizeof(doneText));
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
  // NBNS отвечает за старое разрешение имени, а WS-Discovery — за плитку
  // компьютера в современном разделе «Сеть» Проводника Windows.
  const bool wsdStarted = wsDiscovery.begin(hostname, workgroup, server.guid,
                                             ZIFI_BUILD_VERSION);
  diagnosticLogEvent("DISCOVERY start nbns=%u wsd=%u",
                     discoveryRunning ? 1U : 0U, wsdStarted ? 1U : 0U);
}

void SmbServer::Impl::stopDiscovery() {
  if (discoveryRunning || wsDiscovery.running()) {
    diagnosticLogEvent("DISCOVERY stop nbns=%u wsd=%u",
                       discoveryRunning ? 1U : 0U,
                       wsDiscovery.running() ? 1U : 0U);
  }
  wsDiscovery.stop();
  if (discoveryRunning) {
    udp.stop();
  }
  discoveryRunning = false;
}

void SmbServer::Impl::pollDiscovery() {
  if (!runningFlag) {
    return;
  }
  wsDiscovery.poll();
  if (!discoveryRunning) {
    return;
  }
  const int packetLength = udp.parsePacket();
  if (packetLength <= 0) {
    return;
  }
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
  writeBe32(response + output, 300);           // TTL пять минут
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

bool SmbServer::Impl::requestVfs(VfsOperation operation, const char* path,
                                 uint32_t value, VfsResult& result,
                                 uint32_t timeoutMs) {
  memset(&result, 0, sizeof(result));
  if (!bridge.submit(operation, path, value)) {
    snprintf(lastVfsError, sizeof(lastVfsError), "bridge-busy");
    diagnosticLogEvent("SMB vfs-submit-fail op=%u error=%s",
                       static_cast<unsigned>(operation), lastVfsError);
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
        "SMB vfs-at-submit-fail op=%u off=%lu len=%lu error=%s",
        static_cast<unsigned>(operation), static_cast<unsigned long>(offset),
        static_cast<unsigned long>(length), lastVfsError);
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
  const int slot = activeSlot;
  const ActiveMode mode = activeMode;
  activeSlot = -1;
  activeMode = ActiveMode::kNone;
  activeLogicalOffset = 0;
  activeVfsOffset = 0;
  if (mode == ActiveMode::kDirectory) {
    return true;
  }

  VfsResult result;
  const VfsOperation operation = commit ? VfsOperation::kCloseCommit
                                        : VfsOperation::kCloseAbort;
  const bool closed = requestVfs(operation, nullptr, 0, result,
                                 kMutateVfsTimeoutMs);
  if (!closed && commit) {
    requestVfs(VfsOperation::kCloseAbort, nullptr, 0, result,
               kMutateVfsTimeoutMs);
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
  if (splitParent(path, parent, name) && directoryCache.contains(parent)) {
    DirectoryCache::EntryView cached;
    if (directoryCache.findEntry(parent, name, cached)) {
      result.success = true;
      result.isDirectory = cached.isDirectory;
      result.size = cached.size;
      snprintf(result.name, sizeof(result.name), "%s", cached.name);
      snprintf(lastVfsError, sizeof(lastVfsError), "none");
      return true;
    }
  }
  // Если дескриптор и снимок каталога не дали попадания — проверяем на Z80.
  // Трогать файловый контекст во время активной передачи нельзя: closeActive
  // закрыл бы открытый файл посреди окна. Ждём освобождения канала.
  const uint32_t waitStarted = millis();
  while ((asyncReadCount != 0 || asyncWriteCount != 0 ||
          activeAsyncRead >= 0 || activeAsyncWrite >= 0) &&
         static_cast<uint32_t>(millis() - waitStarted) < kBridgeWaitMs) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  if (asyncReadCount != 0 || asyncWriteCount != 0 || activeAsyncRead >= 0 ||
      activeAsyncWrite >= 0) {
    snprintf(lastVfsError, sizeof(lastVfsError), "bridge-busy");
    return false;
  }
  if (!closeActive(true)) {
    return false;
  }
  const bool statOk = requestVfs(VfsOperation::kStat, path, 0, result,
                                 kNormalVfsTimeoutMs);
  if (statOk) {
    invalidateParent(path);
  }
  return statOk;
}

bool SmbServer::Impl::createEmptyFile(const char* path) {
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
    invalidateFsInfo();
    invalidateParent(path);
    invalidateSubtree(path);
  }
  return removed;
}

SmbServer::Impl::CacheLoadResult
SmbServer::Impl::ensureDirectoryCached(const char* path) {
  if (directoryCache.contains(path)) {
    return CacheLoadResult::kReady;
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

  // Без PSRAM или при переполненном кэше проверяем каталог напрямую. Z80
  // отдаёт конец каталога отдельным успешным результатом с atEnd=true.
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
    handle.directoryCursor = {};
    if (activeSlot == static_cast<int>(index) &&
        activeMode == ActiveMode::kDirectory) {
      activeSlot = -1;
      activeMode = ActiveMode::kNone;
    }
  }
}

void SmbServer::Impl::invalidateSubtree(const char* path) {
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

bool SmbServer::Impl::loadFsInfo(VfsFsInfo& info, uint8_t& status) {
  status = 0;
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
  if ((cachedFsInfo.flags & 0x04) == 0 ||
      cachedFsInfo.freeClusters > cachedFsInfo.totalClusters) {
    cachedFsInfo.freeClusters = cachedFsInfo.totalClusters / 2;
    cachedFsInfo.flags |= 0x04;
  }
  fsInfoValid = true;
  info = cachedFsInfo;
  return true;
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
      const unsigned char value =
          static_cast<unsigned char>(cursor[componentLength]);
      if (value < 32 || value == ':' || value == '|' || value == '<' ||
          value == '>' || value == '"') {
        return false;
      }
      ++componentLength;
    }
    if (componentLength == 1 && component[0] == '.') {
      // Одиночная точка ничего не меняет.
    } else if (componentLength == 2 && component[0] == '.' &&
               component[1] == '.') {
      // Не разрешаем выход выше корня общей папки.
      return false;
    } else if (componentLength != 0) {
      if (used != 1) {
        if (used >= kMaxPath) {
          return false;
        }
        output[used++] = '/';
      }
      if (componentLength > kMaxPath - used) {
        return false;
      }
      memcpy(output + used, component, componentLength);
      used += componentLength;
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

int SmbServer::Impl::allocateHandle() {
  for (size_t index = 0; index < kHandleCount; ++index) {
    if (handles[index].used) {
      continue;
    }
    Handle& handle = handles[index];
    memset(&handle, 0, sizeof(handle));
    handle.used = true;
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
    const uint8_t fileId[SMB2_FD_SIZE], int* slot) {
  int found = -1;
  if (isCompoundFileId(fileId)) {
    found = lastCreatedSlot;
  } else if (readLe32(fileId) == kFileIdMagic) {
    const uint32_t encodedSlot = readLe32(fileId + 8);
    if (encodedSlot >= 1 && encodedSlot <= kHandleCount) {
      found = static_cast<int>(encodedSlot - 1);
    }
  }
  if (found < 0 || static_cast<size_t>(found) >= kHandleCount ||
      !handles[found].used ||
      (!isCompoundFileId(fileId) &&
       memcmp(handles[found].fileId, fileId, SMB2_FD_SIZE) != 0)) {
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
  if (activeSlot == slot) {
    closeActive(true);
  }
  memset(&handles[slot], 0, sizeof(handles[slot]));
  if (lastCreatedSlot == slot) {
    lastCreatedSlot = -1;
  }
}

uint32_t SmbServer::Impl::visibleSize(const Handle& handle) const {
  return handle.sizeReserved && handle.reservedSize > handle.physicalSize
             ? handle.reservedSize
             : handle.physicalSize;
}

uint64_t SmbServer::Impl::directoryFileId(const char* name) const {
  uint64_t hash = 1469598103934665603ULL;
  while (name != nullptr && *name != 0) {
    hash ^= static_cast<uint8_t>(*name++);
    hash *= 1099511628211ULL;
  }
  return hash;
}

void SmbServer::Impl::fillDirectoryInfo(
    smb2_fileidbothdirectoryinformation& info, uint32_t index,
    bool directory, uint32_t size, const char* name) const {
  memset(&info, 0, sizeof(info));
  info.file_index = index;
  info.end_of_file = size;
  info.allocation_size = allocationSize(size);
  info.file_attributes = directory ? SMB2_FILE_ATTRIBUTE_DIRECTORY
                                   : SMB2_FILE_ATTRIBUTE_ARCHIVE;
  info.file_id = directoryFileId(name);
  info.name = name;
}

bool SmbServer::Impl::fetchReadWindow(Handle& handle) {
  if (activeVfsOffset >= handle.physicalSize) {
    return false;
  }
  const uint32_t remaining = handle.physicalSize - activeVfsOffset;
  const uint32_t wanted = static_cast<uint32_t>(minimum(
      static_cast<size_t>(remaining), VfsClient::kFilexTransferWindowSize));
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
      activeLogicalOffset == offset) {
    return true;
  }
  if (!closeActive(true) || !resetBuffers()) {
    return false;
  }
  VfsResult result;
  if (!requestVfs(VfsOperation::kOpenRandom, handle.path, 0, result,
                  kNormalVfsTimeoutMs)) {
    return false;
  }
  activeSlot = slot;
  activeMode = ActiveMode::kRead;
  activeLogicalOffset = offset;
  activeVfsOffset = offset;
  return true;
}

bool SmbServer::Impl::activateWrite(int slot, uint32_t offset) {
  Handle& handle = handles[slot];
  if (activeSlot == slot && activeMode == ActiveMode::kWrite &&
      activeLogicalOffset == offset) {
    return true;
  }
  if (!closeActive(true) || !resetBuffers()) {
    return false;
  }
  VfsResult result;
  const bool useSequential =
      handle.createdNew && offset == 0 && handle.physicalSize == 0;
  const VfsOperation op =
      useSequential ? VfsOperation::kOpenWrite : VfsOperation::kOpenRandom;
  if (!requestVfs(op, handle.path, 0, result, kMutateVfsTimeoutMs)) {
    return false;
  }
  activeSlot = slot;
  activeMode = ActiveMode::kWrite;
  activeLogicalOffset = offset;
  activeVfsOffset = offset;
  return true;
}

bool SmbServer::Impl::commitReservedSize(int slot) {
  if (slot < 0 || static_cast<size_t>(slot) >= kHandleCount) {
    return false;
  }
  Handle& handle = handles[slot];
  if (!handle.sizeReserved || handle.reservedSize <= handle.physicalSize) {
    handle.sizeReserved = false;
    return true;
  }

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
  handle.physicalSize = target;
  handle.reservedSize = target;
  handle.sizeReserved = false;
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
  }
  if ((metadata.timeMask & 0x02) != 0) {
    pending.accessDate = metadata.accessDate;
  }
  if ((metadata.timeMask & 0x04) != 0) {
    pending.writeTime = metadata.writeTime;
    pending.writeDate = metadata.writeDate;
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
  if (activeSlot == slot && activeMode == ActiveMode::kWrite) {
    requestMetadata(handle.pendingMetadata, result);
  } else if (activateWrite(slot, handle.position)) {
    requestMetadata(handle.pendingMetadata, result);
  }
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
  activeLogicalOffset = 0;
  activeVfsOffset = 0;

  // Любая другая VFS-операция меняет рабочий поток WC. При возврате к каталогу
  // открываем его заново и молча пропускаем уже выданные записи.
  for (uint32_t index = 0; index < handle.directoryIndex; ++index) {
    if (!requestVfs(VfsOperation::kReadDirectory, nullptr, 0, result,
                    kNormalVfsTimeoutMs) ||
        result.atEnd) {
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

bool SmbServer::Impl::queueBufferedWriteReplies() {
  if (asyncWriteCount > kAsyncWriteEarlyReplyDepth) {
    return true;
  }
  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    AsyncWrite& pending = asyncWrites[index];
    if (!pending.used || pending.replied || pending.writeThrough ||
        pending.context == nullptr) {
      continue;
    }
    if (!queueAsyncWriteReply(pending.context, pending.messageId,
                              pending.length)) {
      return false;
    }
    pending.replied = true;
  }
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
      if (!result.success || transferred == 0 ||
          transferred != result.transferred) {
        const bool detached = current.context == nullptr;
        const uint32_t status =
            current.cancelRequested
                ? SMB2_STATUS_CANCELLED
                : (result.status != 0 ? smbStatusFromFilex(result.status)
                                      : SMB2_STATUS_IO_DEVICE_ERROR);
        diagnosticLogEvent(
            "SMB async-read-fail status=%u moved=%lu expected=%lu error=%s",
            static_cast<unsigned>(result.status),
            static_cast<unsigned long>(result.transferred),
            static_cast<unsigned long>(current.windowLength), result.error);
        failAsyncReads(status);
        if (detached) {
          releaseClientHandles();
          diagnosticLogEvent("SMB cleanup-deferred done");
        }
        return;
      }
      activeVfsOffset += transferred;
      current.windowLength = 0;
      current.lastProgressMs = millis();
    }

    if (handle == nullptr || current.cancelRequested ||
        current.context == nullptr) {
      const bool detached = current.context == nullptr;
      failAsyncReads(current.cancelRequested ? SMB2_STATUS_CANCELLED
                                             : SMB2_STATUS_IO_DEVICE_ERROR);
      if (detached) {
        releaseClientHandles();
        diagnosticLogEvent("SMB cleanup-deferred done");
      }
      return;
    }

    const size_t ready = bridge.vfsToNetworkAvailable();
    if (ready != 0 && current.filled < current.length) {
      const size_t part = minimum(
          ready, static_cast<size_t>(current.length - current.filled));
      const size_t got = bridge.readForNetwork(
          asyncIoBuffers[activeAsyncRead] + current.filled, part);
      if (got == 0) {
        failAsyncReads(SMB2_STATUS_IO_DEVICE_ERROR);
        return;
      }
      current.filled += static_cast<uint32_t>(got);
      activeLogicalOffset += static_cast<uint32_t>(got);
      current.lastProgressMs = millis();
    }

    if (current.filled == current.length) {
      smb2_context* completedContext = current.context;
      const uint64_t completedMessageId = current.messageId;
      const uint32_t completedLength = current.length;
      const int completedIndex = activeAsyncRead;
      handle->position = current.offset + current.filled;
      activeAsyncRead = -1;
      if (!queueAsyncReadReply(completedContext, completedMessageId,
                               asyncIoBuffers[completedIndex],
                               completedLength)) {
        current = {};
        if (asyncReadCount != 0) {
          --asyncReadCount;
        }
        failAsyncReads(SMB2_STATUS_IO_DEVICE_ERROR);
        smb2_close_context(completedContext);
        return;
      }
      diagnosticLogEvent("SMB read-ok bytes=%lu left=%lu path=%s",
                         static_cast<unsigned long>(completedLength),
                         static_cast<unsigned long>(handle->physicalSize -
                                                    handle->position),
                         handle->path);
      sendOperation("READ", handle->path);
      current = {};
      if (asyncReadCount != 0) {
        --asyncReadCount;
      }
      return;
    }

    if (bridge.requestPending()) {
      return;
    }
    if (activeVfsOffset >= handle->physicalSize) {
      failAsyncReads(SMB2_STATUS_IO_DEVICE_ERROR);
      return;
    }
    const size_t wanted = minimum(
        minimum(static_cast<size_t>(current.length - current.filled),
                static_cast<size_t>(handle->physicalSize - activeVfsOffset)),
        minimum(VfsClient::kFilexTransferWindowSize,
                bridge.vfsToNetworkFree()));
    if (wanted == 0 ||
        !bridge.submitAt(VfsOperation::kReadAt, activeVfsOffset,
                         static_cast<uint32_t>(wanted))) {
      failAsyncReads(SMB2_STATUS_IO_DEVICE_ERROR);
      return;
    }
    current.windowLength = static_cast<uint32_t>(wanted);
    current.inFlight = true;
    return;
  }

  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    if (asyncReads[index].used && asyncReads[index].cancelRequested) {
      const bool detached = asyncReads[index].context == nullptr;
      failAsyncReads(SMB2_STATUS_CANCELLED);
      if (detached) {
        releaseClientHandles();
      }
      return;
    }
  }

  const int nextIndex = findReadyAsyncRead();
  if (nextIndex < 0) {
    return;
  }
  AsyncRead& next = asyncReads[nextIndex];
  if (bridge.requestPending() || !activateRead(next.slot, next.offset)) {
    failAsyncReads(SMB2_STATUS_IO_DEVICE_ERROR);
    return;
  }
  activeAsyncRead = nextIndex;
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
        noteFileGrowth(handle->physicalSize, end);
        handle->physicalSize = end;
        refreshCachedSize(handle->path, end);
      }
      handle->metadataDirty = true;
      handle->position = end;
      activeLogicalOffset = end;
      activeVfsOffset = end;
      if (handle->sizeReserved &&
          handle->physicalSize >= handle->reservedSize) {
        handle->sizeReserved = false;
      }
    }

    const bool succeeded = result.success && handle != nullptr &&
                           completed.windowLength != 0 &&
                           transferred == completed.windowLength &&
                           !completed.cancelRequested &&
                           (completed.replied || completed.context != nullptr);
    if (!succeeded) {
      const bool detached = completed.context == nullptr;
      const uint32_t status =
          completed.cancelRequested
              ? SMB2_STATUS_CANCELLED
              : (result.status != 0 ? smbStatusFromFilex(result.status)
                                    : SMB2_STATUS_IO_DEVICE_ERROR);
      failAsyncWrites(status);
      if (detached) {
        releaseClientHandles();
        diagnosticLogEvent("SMB cleanup-deferred done");
      }
      return;
    }

    completed.flushed += transferred;
    completed.inFlight = false;
    completed.windowLength = 0;

    const bool finished = completed.flushed == completed.length;
    // WRITE_THROUGH и запросы, удержанные обратным давлением, обязаны получить
    // ответ не позднее полного физического завершения.
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
      sendOperation("WRITE", handle->path);
      completed = {};
      if (asyncWriteCount != 0) {
        --asyncWriteCount;
      }
      if (!queueBufferedWriteReplies()) {
        failAsyncWrites(SMB2_STATUS_IO_DEVICE_ERROR);
        if (client != nullptr) {
          smb2_close_context(client);
        }
        return;
      }
      if (detached && asyncWriteCount == 0 && asyncReadCount == 0) {
        closeActive(true);
        releaseClientHandles();
        diagnosticLogEvent("SMB cleanup-deferred done");
      }
    }
  }

  for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
    if (asyncWrites[index].used && asyncWrites[index].cancelRequested) {
      const bool detached = asyncWrites[index].context == nullptr;
      failAsyncWrites(SMB2_STATUS_CANCELLED);
      if (detached) {
        releaseClientHandles();
      }
      return;
    }
  }

  const int nextIndex = findReadyAsyncWrite();
  if (nextIndex < 0) {
    return;
  }
  AsyncWrite& next = asyncWrites[nextIndex];
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
  activeAsyncWrite = nextIndex;
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
  diagnosticLogEvent("SMB destruction client=%p owner=%u", smb2,
                     self != nullptr && self->client == smb2 ? 1U : 0U);
  if (self != nullptr) {
    self->cleanupClient(smb2);
  }
  return 0;
}

int SmbServer::Impl::serviceHandler(smb2_server* serverValue) {
  Impl* self = from(serverValue);
  if (self != nullptr) {
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
    self->pollAsyncWrite();
  }
  return 0;
}

int SmbServer::Impl::authorizeHandler(smb2_server* serverValue,
                                      smb2_context* smb2,
                                      const char* requestedUser,
                                      const char*, const char*) {
  Impl* self = from(serverValue);
  if (self == nullptr || requestedUser == nullptr ||
      !asciiEqualNoCase(requestedUser, self->user)) {
    diagnosticLogEvent("SMB auth-denied user=%s",
                       requestedUser == nullptr ? "(null)" : requestedUser);
    return -1;
  }
  diagnosticLogEvent("SMB auth-accepted user=%s", requestedUser);
  smb2_set_user(smb2, self->user);
  smb2_set_domain(smb2, self->workgroup);
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
  if (self != nullptr && self->client == smb2) {
    diagnosticLogEvent("SMB logoff");
    // LOGOFF завершает сеанс, но не обязан закрывать сам TCP-сокет. Поэтому
    // указатель client сохраняем: иначе второй компьютер смог бы войти, пока
    // первый контекст libsmb2 ещё жив. Новый SESSION_SETUP на этом же сокете
    // снова пройдёт обычную авторизацию.
    self->closeActive(true);
    for (size_t index = 0; index < kHandleCount; ++index) {
      if (self->handles[index].used &&
          self->handles[index].deletePending &&
          strcmp(self->handles[index].path, "/") != 0) {
        self->removePath(self->handles[index].path);
      } else if (self->handles[index].used &&
                 self->handles[index].metadataDirty) {
        self->invalidateParent(self->handles[index].path);
      }
    }
    self->resetHandles();
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
  reply->tree_id = self->allocateTree(ipc);
  if (reply->tree_id == 0) {
    return replyStatus(smb2, SMB2_TREE_CONNECT,
                       SMB2_STATUS_INSUFFICIENT_RESOURCES);
  }
  // Кэширование отключено: физический VFS односессионный и не рассылает
  // уведомления об изменениях другим SMB-клиентам.
  reply->share_flags = SMB2_SHAREFLAG_NO_CACHING;
  reply->capabilities = 0;
  self->sendOperation("TREE", requestedShare);
  return 0;
}

int SmbServer::Impl::treeDisconnectHandler(smb2_server* serverValue,
                                           smb2_context*, uint32_t treeId) {
  Impl* self = from(serverValue);
  if (self != nullptr) {
    self->closeActive(true);
    self->releaseTree(treeId);
    self->sendOperation("UNTREE");
  }
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
  // A related compound request uses an all-ones FileId to refer to the
  // immediately preceding successful CREATE.  Clear the alias before doing
  // any work so a failed CREATE followed by CLOSE cannot close an older
  // handle left by a previous compound request.
  self->lastCreatedSlot = -1;
  diagnosticLogEvent(
      "SMB create-enter tree=%08lx disp=%lu opts=%08lx name=%s",
      static_cast<unsigned long>(smb2_get_current_tree_id(smb2)),
      static_cast<unsigned long>(request->create_disposition),
      static_cast<unsigned long>(request->create_options),
      request->name == nullptr ? "(null)" : request->name);
  const Tree* tree = self->findTree(smb2_get_current_tree_id(smb2));
  if (tree == nullptr) {
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
    const int slot = self->allocateHandle();
    if (slot < 0) {
      return createStatus(smb2, request,
                          SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    Handle& handle = self->handles[slot];
    handle.pipe = true;
    handle.rpcContextId = 0xFFFF;
    snprintf(handle.path, sizeof(handle.path), "srvsvc");
    memcpy(reply->file_id, handle.fileId, SMB2_FD_SIZE);
    self->lastCreatedSlot = slot;
    reply->oplock_level = SMB2_OPLOCK_LEVEL_NONE;
    reply->create_action = kCreateOpened;
    reply->file_attributes = SMB2_FILE_ATTRIBUTE_NORMAL;
    self->sendOperation("PIPE", "srvsvc");
    return 0;
  }

  char path[kMaxPath + 1];
  if (!self->normalizePath(request->name == nullptr ? "" : request->name,
                           path) ||
      request->create_disposition > SMB2_FILE_OVERWRITE_IF) {
    return createStatus(smb2, request, SMB2_STATUS_OBJECT_NAME_INVALID);
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
  const bool explicitDirectory =
      (request->create_options & SMB2_FILE_DIRECTORY_FILE) != 0;
  const bool explicitFile =
      (request->create_options & SMB2_FILE_NON_DIRECTORY_FILE) != 0;
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
      for (size_t i = 0; i < kHandleCount; ++i) {
        if (self->handles[i].used && asciiEqualNoCase(self->handles[i].path, path)) {
          self->handles[i].physicalSize = 0;
          self->handles[i].position = 0;
          self->handles[i].openedSize = 0;
        }
      }
      size = 0;
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
    }
    size = 0;
    action = kCreateCreated;
  }

  const int slot = self->allocateHandle();
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
  handle.physicalSize = size;
  handle.openedSize = size;
  handle.reservedSize = size;
  snprintf(handle.path, sizeof(handle.path), "%s", path);
  memcpy(reply->file_id, handle.fileId, SMB2_FD_SIZE);
  self->lastCreatedSlot = slot;

  reply->oplock_level = SMB2_OPLOCK_LEVEL_NONE;
  reply->create_action = action;
  reply->allocation_size = allocationSize(size);
  reply->end_of_file = size;
  reply->file_attributes = directory ? SMB2_FILE_ATTRIBUTE_DIRECTORY
                                     : SMB2_FILE_ATTRIBUTE_ARCHIVE;
  diagnosticLogEvent("SMB create-ok slot=%d dir=%u size=%lu path=%s", slot,
                     directory ? 1U : 0U,
                     static_cast<unsigned long>(size), path);
  self->sendOperation("OPEN", path);
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
                       : self->findHandle(request->file_id, &slot);
  if (handle == nullptr) {
    return replyStatus(smb2, SMB2_CLOSE, SMB2_STATUS_FILE_CLOSED);
  }
  if (!self->drainAsyncWritesForHandle(slot, handle->generation)) {
    handle->failed = true;
  }
  bool resizeFailed = false;
  if (!handle->failed && handle->sizeReserved &&
      !self->commitReservedSize(slot)) {
    // Не оставляем в ответах логический размер, который не удалось записать на
    // SD. Исходный файл при этом не удаляем: ошибка EXTEND не равна сбою
    // создания нового файла.
    handle->sizeReserved = false;
    resizeFailed = true;
  }
  bool metadataFailed = false;
  if (!handle->failed && handle->metadataPending &&
      !self->applyPendingMetadata(slot)) {
    // Ошибка времени/атрибутов не должна удалять уже полностью записанный файл.
    // CLOSE всё равно закрывает VFS, но сообщает Windows о сбое метаданных.
    handle->metadataPending = false;
    metadataFailed = true;
  }
  const bool wasActive = self->activeSlot == slot;
  if (handle->createdNew && handle->physicalSize == 0 && !wasActive &&
      !handle->directory && !handle->failed) {
    self->createEmptyFile(handle->path);
  }
  if (wasActive && handle->writable && !self->closeActive(!handle->failed)) {
    handle->failed = true;
  } else if (wasActive && !handle->writable) {
    self->closeActive(true);
  }

  memset(reply, 0, sizeof(*reply));
  if ((request->flags & SMB2_CLOSE_FLAG_POSTQUERY_ATTRIB) != 0) {
    reply->flags = SMB2_CLOSE_FLAG_POSTQUERY_ATTRIB;
    reply->allocation_size = allocationSize(handle->physicalSize);
    reply->end_of_file = handle->physicalSize;
    reply->file_attributes = handle->directory
                                 ? SMB2_FILE_ATTRIBUTE_DIRECTORY
                                 : SMB2_FILE_ATTRIBUTE_ARCHIVE;
  }

  const bool writeFailed = handle->failed;
  bool removed = true;
  const bool shouldDelete = handle->deletePending ||
                            (handle->failed && handle->createdNew);
  if (shouldDelete && strcmp(handle->path, "/") != 0) {
    removed = self->removePath(handle->path);
  } else if (handle->metadataDirty) {
    // Снимок родителя во время записи обновлялся точечно, но окончательную
    // длину и занятое место подтверждает только CLOSE. Здесь же — то самое
    // единственное уведомление о суммарном изменении длины файла.
    self->invalidateParent(handle->path);
  }
  if (!handle->directory && handle->position != 0) {
    // Итоговая строка обязана пройти без ограничения частоты, иначе счётчик
    // замрёт на предпоследнем блоке и файл будет выглядеть недокопированным.
    self->sendProgress(*handle, handle->deletePending ? "DELETE" : "DONE",
                       true);
  }
  self->sendOperation(handle->deletePending ? "DELETE" : "CLOSE",
                      handle->path);
  self->releaseHandle(slot);
  if (!removed) {
    return replyStatus(smb2, SMB2_CLOSE, SMB2_STATUS_ACCESS_DENIED);
  }
  return resizeFailed || writeFailed
             ? replyStatus(smb2, SMB2_CLOSE, SMB2_STATUS_IO_DEVICE_ERROR)
             : 0;
}

int SmbServer::Impl::flushHandler(smb2_server* serverValue,
                                  smb2_context* smb2,
                                  smb2_flush_request* request) {
  Impl* self = from(serverValue);
  int slot = -1;
  Handle* handle = self == nullptr || request == nullptr
                       ? nullptr
                       : self->findHandle(request->file_id, &slot);
  if (handle == nullptr) {
    return replyStatus(smb2, SMB2_FLUSH, SMB2_STATUS_INVALID_HANDLE);
  }
  if (!self->drainAsyncWritesForHandle(slot, handle->generation)) {
    handle->failed = true;
    return replyStatus(smb2, SMB2_FLUSH, SMB2_STATUS_IO_DEVICE_ERROR);
  }
  if (handle->sizeReserved && !self->commitReservedSize(slot)) {
    handle->sizeReserved = false;
    return replyStatus(smb2, SMB2_FLUSH, SMB2_STATUS_IO_DEVICE_ERROR);
  }
  if (handle->metadataPending) {
    self->applyPendingMetadata(slot);
  }
  if (self->activeSlot == slot && self->activeMode == ActiveMode::kWrite &&
      !self->closeActive(true)) {
    handle->failed = true;
    return replyStatus(smb2, SMB2_FLUSH, SMB2_STATUS_IO_DEVICE_ERROR);
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
                       : self->findHandle(request->file_id, &slot);
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
      request->minimum_count > kSmbAdvertisedIoSize) {
    return replyStatus(smb2, SMB2_READ, SMB2_STATUS_INVALID_PARAMETER);
  }
  if (self->asyncWriteCount != 0) {
    return replyStatus(smb2, SMB2_READ,
                       SMB2_STATUS_INSUFFICIENT_RESOURCES);
  }
  if (handle->sizeReserved && !self->commitReservedSize(slot)) {
    handle->sizeReserved = false;
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
  const uint32_t physicalWindow = static_cast<uint32_t>(minimum(
      self->bridge.ringCapacity(), VfsClient::kFilexTransferWindowSize));
  if (physicalWindow == 0) {
    return replyStatus(smb2, SMB2_READ,
                       SMB2_STATUS_INSUFFICIENT_RESOURCES);
  }
  const uint32_t wanted = static_cast<uint32_t>(minimum64(
      minimum64(request->length, kSmbAdvertisedIoSize),
      static_cast<uint64_t>(handle->physicalSize - offset)));
  if (wanted < request->minimum_count) {
    return replyStatus(smb2, SMB2_READ, SMB2_STATUS_END_OF_FILE);
  }
  if (wanted == 0) {
    reply->data = nullptr;
    reply->data_length = 0;
    reply->data_remaining = 0;
    return 0;
  }

  // Финальный READ подтверждает весь сетевой запрос. Промежуточный
  // STATUS_PENDING здесь намеренно не отправляется: он немедленно возвращал
  // Windows кредит, Explorer накапливал около пяти запросов и долго держал
  // индикатор на нуле.
  // Windows CopyFile не повторяет хвост короткого успешного READ: аппаратная
  // проверка оставляла только 16 352 байта на каждый 256-КиБ блок файла.
  const bool needsAsync = wanted >= physicalWindow ||
                          self->asyncReadCount != 0 ||
                          offset != handle->position;
  if (needsAsync) {
    const uint64_t messageId = smb2_get_last_request_message_id(smb2);
    if (messageId == 0) {
      return replyStatus(smb2, SMB2_READ,
                         SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    const int asyncIndex = self->allocateAsyncRead();
    if (asyncIndex < 0) {
      return replyStatus(smb2, SMB2_READ,
                         SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    AsyncRead& pending = self->asyncReads[asyncIndex];
    pending.used = true;
    pending.context = smb2;
    pending.messageId = messageId;
    pending.slot = slot;
    pending.generation = handle->generation;
    pending.offset = offset;
    pending.length = wanted;
    pending.lastProgressMs = millis();
    ++self->asyncReadCount;
    self->pollAsyncRead();
    return 1;
  }
  if (!self->activateRead(slot, offset)) {
    return replyStatus(smb2, SMB2_READ, SMB2_STATUS_IO_DEVICE_ERROR);
  }

  uint8_t* data = static_cast<uint8_t*>(malloc(wanted));
  if (data == nullptr) {
    return replyStatus(smb2, SMB2_READ,
                       SMB2_STATUS_INSUFFICIENT_RESOURCES);
  }
  uint32_t copied = 0;
  while (copied < wanted) {
    const size_t ready = self->bridge.vfsToNetworkAvailable();
    if (ready == 0) {
      if (!self->fetchReadWindow(*handle)) {
        free(data);
        self->closeActive(false);
        return replyStatus(smb2, SMB2_READ, SMB2_STATUS_IO_DEVICE_ERROR);
      }
      continue;
    }
    const size_t part = minimum(ready, static_cast<size_t>(wanted - copied));
    const size_t got = self->bridge.readForNetwork(data + copied, part);
    if (got == 0) {
      free(data);
      self->closeActive(false);
      return replyStatus(smb2, SMB2_READ, SMB2_STATUS_IO_DEVICE_ERROR);
    }
    copied += static_cast<uint32_t>(got);
    self->activeLogicalOffset += static_cast<uint32_t>(got);
  }
  handle->position = offset + copied;
  reply->data = data;  // После отправки этот malloc освободит сама libsmb2.
  reply->data_length = copied;
  // Для обычного TCP Channel=NONE, поэтому это не число байтов до EOF.
  reply->data_remaining = 0;
  diagnosticLogEvent("SMB read-ok bytes=%lu left=%lu path=%s",
                     static_cast<unsigned long>(copied),
                     static_cast<unsigned long>(handle->physicalSize -
                                                handle->position),
                     handle->path);
  self->sendOperation("READ", handle->path);
  return 0;
}

int SmbServer::Impl::writeHandler(smb2_server* serverValue,
                                  smb2_context* smb2,
                                  smb2_write_request* request,
                                  smb2_write_reply* reply) {
  Impl* self = from(serverValue);
  int slot = -1;
  Handle* handle = self == nullptr || request == nullptr
                       ? nullptr
                       : self->findHandle(request->file_id, &slot);
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
      request->length > kSmbAdvertisedIoSize ||
      request->length > UINT32_MAX -
                                static_cast<uint32_t>(request->offset) ||
      (request->length != 0 && request->buf == nullptr)) {
    return replyStatus(smb2, SMB2_WRITE, SMB2_STATUS_INVALID_PARAMETER);
  }
  if (self->asyncReadCount != 0) {
    return replyStatus(smb2, SMB2_WRITE,
                       SMB2_STATUS_INSUFFICIENT_RESOURCES);
  }
  const uint32_t offset = static_cast<uint32_t>(request->offset);
  if (request->length == 0) {
    reply->count = 0;
    reply->remaining = 0;
    return 0;
  }

  // Payload копируется в независимый PSRAM-слот до возврата из callback.
  // FILEX WRITE_AT сохраняет реальный 32-битный offset каждого запроса.
  const size_t window = minimum(self->bridge.ringCapacity(),
                                static_cast<size_t>(
                                    VfsClient::kTransferWindowSize));
  // Windows CopyFile не повторяет хвост короткого успешного WRITE. Поэтому весь
  // запрос копируется в PSRAM и всегда подтверждается полной длиной. Обычный
  // WRITE получает write-back ответ сразу из PSRAM; WRITE_THROUGH ждёт SD.
  // При заполнении очереди последние три ответа удерживают исходные кредиты и
  // не дают Windows прислать больше payload, чем помещается в фиксированный пул.
  const bool needsAsync = request->length > window ||
                          self->asyncWriteCount != 0;
  if (needsAsync) {
    const uint64_t messageId = smb2_get_last_request_message_id(smb2);
    if (messageId == 0) {
      return replyStatus(smb2, SMB2_WRITE,
                         SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    const int asyncIndex = self->allocateAsyncWrite();
    if (asyncIndex < 0) {
      return replyStatus(smb2, SMB2_WRITE,
                         SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    AsyncWrite& pending = self->asyncWrites[asyncIndex];
    pending.used = true;
    pending.inFlight = false;
    pending.cancelRequested = false;
    pending.replied = false;
    pending.writeThrough =
        (request->flags & SMB2_WRITEFLAG_WRITE_THROUGH) != 0;
    pending.context = smb2;
    pending.messageId = messageId;
    pending.slot = slot;
    pending.generation = handle->generation;
    pending.offset = offset;
    pending.length = request->length;
    pending.flushed = 0;
    pending.windowLength = 0;
    pending.lastProgressMs = millis();
    memcpy(self->asyncIoBuffers[asyncIndex], request->buf,
            request->length);
    ++self->asyncWriteCount;
    if (!self->queueBufferedWriteReplies()) {
      self->failAsyncWrites(SMB2_STATUS_IO_DEVICE_ERROR);
      smb2_close_context(smb2);
      return 1;
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
    written += static_cast<uint32_t>(part);
    const uint32_t end = offset + written;
    if (end > handle->physicalSize) {
      self->noteFileGrowth(handle->physicalSize, end);
      handle->physicalSize = end;
      self->refreshCachedSize(handle->path, end);
    }
    handle->metadataDirty = true;
    handle->position = end;
    self->activeLogicalOffset = end;
    self->activeVfsOffset = end;
    if (handle->sizeReserved &&
        handle->physicalSize >= handle->reservedSize) {
      handle->sizeReserved = false;
    }
  }
  reply->count = request->length;
  reply->remaining = 0;
  self->sendOperation("WRITE", handle->path);
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

int SmbServer::Impl::lockHandler(smb2_server*, smb2_context*,
                                 smb2_lock_request*) {
  // Реальных параллельных клиентов нет, поэтому локальный одноклиентский
  // сервер может безопасно подтверждать byte-range lock без отдельной таблицы.
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
    Handle* handle = self->findHandle(request->file_id);
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
  if (self != nullptr) {
    for (size_t index = 0; index < kAsyncIoQueueDepth; ++index) {
      AsyncRead& read = self->asyncReads[index];
      if (read.used && read.context == smb2) {
        read.cancelRequested = true;
      }
      AsyncWrite& pending = self->asyncWrites[index];
      if (pending.used && pending.context == smb2) {
        // Уже выполняющийся APPEND нельзя оборвать посередине UART-кадра.
        // После ACK текущего окна отменяем всю зависимую последовательность.
        pending.cancelRequested = true;
      }
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
                              static_cast<uint32_t>(kSmbAdvertisedIoSize));
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
                      cached.size, cached.name);
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

int SmbServer::Impl::queryDirectoryHandler(
    smb2_server* serverValue, smb2_context* smb2,
    smb2_query_directory_request* request,
    smb2_query_directory_reply* reply) {
  Impl* self = from(serverValue);
  int slot = -1;
  Handle* handle = self == nullptr || request == nullptr
                       ? nullptr
                       : self->findHandle(request->file_id, &slot);
  if (handle == nullptr || !handle->directory) {
    return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                       SMB2_STATUS_NOT_A_DIRECTORY);
  }
  if (request->file_information_class !=
          SMB2_FILE_FULL_DIRECTORY_INFORMATION &&
      request->file_information_class !=
          SMB2_FILE_BOTH_DIRECTORY_INFORMATION &&
      request->file_information_class !=
          SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION &&
      request->file_information_class !=
          SMB2_FILE_ID_FULL_DIRECTORY_INFORMATION) {
    return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                       SMB2_STATUS_INVALID_PARAMETER);
  }

  if ((request->flags & (SMB2_RESTART_SCANS | SMB2_REOPEN)) != 0) {
    handle->directoryIndex = 0;
    handle->directoryEnded = false;
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
  VfsResult result;
  CacheLoadResult cacheResult = CacheLoadResult::kUnavailable;
  if (self->directoryCache.contains(handle->path)) {
    cacheResult = CacheLoadResult::kReady;
  } else if (!handle->directoryCacheUnavailable) {
    cacheResult = self->ensureDirectoryCached(handle->path);
    if (cacheResult == CacheLoadResult::kUnavailable) {
      // Если конкретный снимок не поместился, не перечитываем весь каталог
      // заново перед каждым следующим элементом этого же HANDLE.
      handle->directoryCacheUnavailable = true;
    }
  }
  if (cacheResult == CacheLoadResult::kIoError) {
    return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                       SMB2_STATUS_IO_DEVICE_ERROR);
  }
  if (cacheResult == CacheLoadResult::kReady) {
    return self->queryCachedDirectory(smb2, *handle, request, reply);
  }

  // PSRAM отсутствует либо единственный каталог не поместился в лимит.
  // Функциональность не теряется: остаётся старый потоковый путь через UART.
  if (!self->activateDirectory(slot)) {
    return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                       SMB2_STATUS_IO_DEVICE_ERROR);
  }
  while (true) {
    if (!self->requestVfs(VfsOperation::kReadDirectory, nullptr, 0, result,
                          kNormalVfsTimeoutMs)) {
      return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                         SMB2_STATUS_IO_DEVICE_ERROR);
    }
    if (result.atEnd) {
      handle->directoryEnded = true;
      return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                         SMB2_STATUS_NO_MORE_FILES);
    }
    ++handle->directoryIndex;
    if (wildcardMatch(handle->pattern, result.name)) {
      break;
    }
  }

  const size_t encodedEstimate =
      directoryEncodedSize(request->file_information_class, result.name);
  if (request->output_buffer_length < encodedEstimate) {
    return replyStatus(smb2, SMB2_QUERY_DIRECTORY,
                       SMB2_STATUS_BUFFER_TOO_SMALL);
  }

  memset(&self->directoryInfo, 0, sizeof(self->directoryInfo));
  snprintf(self->directoryName, sizeof(self->directoryName), "%s",
           result.name);
  self->fillDirectoryInfo(self->directoryInfo, handle->directoryIndex,
                          result.isDirectory, result.size,
                          self->directoryName);
  reply->output_buffer =
      reinterpret_cast<uint8_t*>(&self->directoryInfo);
  reply->output_buffer_length = static_cast<uint32_t>(
      padTo8(sizeof(self->directoryInfo)));
  self->sendOperation("DIR", handle->path);
  return 0;
}

int SmbServer::Impl::changeNotifyHandler(
    smb2_server*, smb2_context* smb2, smb2_change_notify_request*,
    smb2_change_notify_reply*) {
  // UART/VFS не присылает события FAT. Немедленный NOT_SUPPORTED лучше
  // вечного зависшего запроса проводника.
  return replyStatus(smb2, SMB2_CHANGE_NOTIFY, SMB2_STATUS_NOT_SUPPORTED);
}

int SmbServer::Impl::queryInfoHandler(smb2_server* serverValue,
                                      smb2_context* smb2,
                                      smb2_query_info_request* request,
                                      smb2_query_info_reply* reply) {
  Impl* self = from(serverValue);
  Handle* handle = self == nullptr || request == nullptr
                       ? nullptr
                       : self->findHandle(request->file_id);
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
  const uint32_t attributes = handle->directory
                                  ? SMB2_FILE_ATTRIBUTE_DIRECTORY
                                  : SMB2_FILE_ATTRIBUTE_ARCHIVE;
  void* output = nullptr;
  uint32_t outputLength = 0;

  if (request->info_type == SMB2_0_INFO_FILE) {
    switch (request->file_info_class) {
      case SMB2_FILE_BASIC_INFORMATION:
        memset(&self->basicInfo, 0, sizeof(self->basicInfo));
        self->basicInfo.file_attributes = attributes;
        output = &self->basicInfo;
        outputLength = sizeof(self->basicInfo);
        break;
      case SMB2_FILE_STANDARD_INFORMATION:
        memset(&self->standardInfo, 0, sizeof(self->standardInfo));
        self->standardInfo.allocation_size = allocationSize(size);
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
      case SMB2_FILE_ALL_INFORMATION:
        memset(&self->allInfo, 0, sizeof(self->allInfo));
        if (!self->makeInfoName(*handle)) {
          break;
        }
        self->allInfo.basic.file_attributes = attributes;
        self->allInfo.standard.allocation_size = allocationSize(size);
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
      case SMB2_FILE_INTERNAL_INFORMATION:
        memset(&self->internalInfo, 0, sizeof(self->internalInfo));
        self->internalInfo.index_number = self->directoryFileId(handle->path);
        output = &self->internalInfo;
        outputLength = sizeof(self->internalInfo);
        break;
      case SMB2_FILE_NETWORK_OPEN_INFORMATION:
        memset(&self->networkInfo, 0, sizeof(self->networkInfo));
        self->networkInfo.allocation_size = allocationSize(size);
        self->networkInfo.end_of_file = size;
        self->networkInfo.file_attributes = attributes;
        output = &self->networkInfo;
        outputLength = sizeof(self->networkInfo);
        break;
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
          // Длина имени задаётся В СИМВОЛАХ: smb2_encode_file_stream_info сама
          // домножает её на два, переводя в байты UTF-16LE. Удвоить здесь —
          // значит получить учетверение, завышенный fslen и битую структуру,
          // на которой Проводник молча замирает после успешного READ.
          self->streamInfo.stream_name_length = strlen("::$DATA");
          self->streamInfo.stream_size = size;
          self->streamInfo.stream_allocation_size = allocationSize(size);
          output = &self->streamInfo;
          outputLength = sizeof(self->streamInfo);
        }
        break;
      default:
        break;
    }
  } else if (request->info_type == SMB2_0_INFO_FILESYSTEM &&
             request->file_info_class == SMB2_FILE_FS_ATTRIBUTE_INFORMATION) {
    // Имя и возможности FAT32 постоянны и не требуют чтения геометрии, а тем
    // более повторного потокового чтения активной FAT после создания файла.
    memset(&self->attributeInfo, 0, sizeof(self->attributeInfo));
    self->attributeInfo.filesystem_attributes = 0x00000042;
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
                       : self->findHandle(request->file_id, &slot);
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
        if (fileTimeToFat(readLe64Local(data), fatDate, fatTime,
                          &metadata.createTenth)) {
          metadata.timeMask |= 0x01;
          metadata.createDate = fatDate;
          metadata.createTime = fatTime;
        }
        if (fileTimeToFat(readLe64Local(data + 8), fatDate, fatTime)) {
          metadata.timeMask |= 0x02;
          metadata.accessDate = fatDate;
        }
        if (fileTimeToFat(readLe64Local(data + 16), fatDate, fatTime)) {
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
          return 0;
        }
        self->deferMetadata(*handle, metadata);
        self->sendOperation("ATTR", handle->path);
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
        handle->reservedSize = requestedSize;
        self->sendOperation("RESERVE", handle->path);
        return 0;
      }
      if (requestedSize == handle->physicalSize) {
        handle->sizeReserved = false;
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
      handle->physicalSize = requestedSize;
      handle->reservedSize = requestedSize;
      handle->sizeReserved = false;
      if (handle->position > requestedSize) {
        handle->position = requestedSize;
      }
      handle->metadataDirty = true;
      self->activeLogicalOffset = handle->position;
      self->activeVfsOffset = handle->position;
      self->sendOperation(requestedSize == 0 ? "TRUNCATE" : "SETEOF",
                          handle->path);
      return 0;
    }

    case SMB2_FILE_RENAME_INFORMATION: {
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
      if (!self->requestMoveRename(handle->path, target, handle->directory,
                                   replace, result)) {
        return replyStatus(
            smb2, SMB2_SET_INFO,
            result.status != 0 ? smbStatusFromFilex(result.status)
                               : SMB2_STATUS_IO_DEVICE_ERROR);
      }
      self->invalidateParent(oldPath);
      self->invalidateParent(target);
      self->invalidateSubtree(oldPath);
      self->invalidateSubtree(target);
      snprintf(handle->path, sizeof(handle->path), "%s", target);
      self->sendOperation("RENAME", target);
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
