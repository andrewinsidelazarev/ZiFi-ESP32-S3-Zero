#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/fat_allocation_cache.hpp"
#include "zifi/uart_transport.hpp"

namespace zifi {

struct VfsEntry {
  bool isDirectory = false;
  uint32_t size = 0;
  char name[256] = {};
};

struct VfsFsInfo {
  uint8_t flags = 0;
  uint8_t sectorsPerCluster = 0;
  uint16_t bytesPerSector = 0;
  uint32_t totalClusters = 0;
  uint32_t freeClusters = 0xFFFFFFFFUL;
  uint32_t serial = 0;
  char label[12] = {};
  uint32_t totalSectors = 0;
  uint32_t nextFree = 0xFFFFFFFFUL;
  uint8_t fatCount = 0;
  uint8_t activeFat = 0;
  uint8_t fatFlags = 0;
  uint8_t mediaState = 0;
  uint32_t fatSectors = 0;
};

struct VfsMetadata {
  uint8_t attrMask = 0;
  uint8_t attrValue = 0;
  uint8_t timeMask = 0;
  uint8_t createTenth = 0;
  uint16_t createTime = 0;
  uint16_t createDate = 0;
  uint16_t accessDate = 0;
  uint16_t writeTime = 0;
  uint16_t writeDate = 0;
  // Эти значения нужны только SMB-серверу для согласованного QUERY_INFO.
  // В UART-пакет FILEX по-прежнему уходят первые 16 байт в формате FAT.
  uint64_t createFileTime = 0;
  uint64_t accessFileTime = 0;
  uint64_t writeFileTime = 0;
};

// Синхронный клиент файлового API Wild Commander. Вызывать его разрешено
// только с ядра 1: он отправляет VFS-команды в UART и ждёт ответы Z80.
class VfsClient {
 public:
  static constexpr size_t kTransferWindowSize = 16 * 1024;
  // FILEX needs a 32-byte parameter block in the same 16 KiB page as data.
  static constexpr size_t kFilexTransferWindowSize =
      kTransferWindowSize - 32;

  using WindowSource = size_t (*)(void* context, size_t offset,
                                  uint8_t* output, size_t capacity);
  using WindowSink = bool (*)(void* context, const uint8_t* data,
                              size_t length);

  explicit VfsClient(UartTransport& transport);

  void beginFatCache(bool psramAvailable);

  bool stat(const char* path, VfsEntry& entry);
  bool openDirectory(const char* path);
  bool readDirectory(VfsEntry& entry, bool& atEnd);

  bool openFile(const char* path, bool forWrite);
  bool openFileAppend(const char* path);
  bool openFileRandom(const char* path);
  bool seekFile(uint32_t offset);
  bool readFile(uint8_t* output, size_t capacity, size_t wanted,
                size_t& received);
  bool readFileWindow(size_t wanted, WindowSink sink, void* sinkContext,
                      size_t& received);
  bool writeFile(const uint8_t* data, size_t length);
  bool writeFileWindow(size_t length, WindowSource source,
                       void* sourceContext);
  bool extendFile(uint32_t length);
  bool setFileSize(uint32_t size, uint32_t& actualSize);
  bool closeFile(bool commit = true);

  // cacheOnly — вернуть ответ, только если он уже есть: вызывающий ещё не
  // закрывал активный файл и по отказу поймёт, что закрыть придётся.
  bool getFsInfo(VfsFsInfo& info, bool refreshFree = false,
                 bool cacheOnly = false);
  // Изменение длины файла: единственный способ сообщить владельцу счётчика,
  // сколько кластеров занято или освобождено, без обхода FAT.
  void noteFileResized(uint32_t oldSize, uint32_t newSize);
  // Полный сброс сведений о томе — для операций, объём которых неизвестен.
  void invalidateFsCache();
  bool moveRename(const char* oldPath, const char* newPath, bool directory,
                  bool replace);
  bool setMetadata(const VfsMetadata& metadata, uint8_t& appliedAttributes);

  bool remove(const char* path);
  bool makeDirectory(const char* path);
  bool rename(const char* oldPath, const char* newName, bool directory);

  const char* lastError() const { return lastError_; }
  uint8_t lastStatus() const { return lastStatus_; }
  uint8_t filexCapabilities() const { return filexCapabilities_; }
  uint8_t openMode() const { return openMode_; }

 private:
  static constexpr size_t kBlockSize = 512;
  static constexpr size_t kFirstData = 248;
  static constexpr size_t kContinueData = 252;
  static constexpr size_t kWriteWindowHeader = 8;
  static constexpr size_t kReadWindowHeader = 9;
  static constexpr size_t kFatTransferWindowSize = 31 * 512;
  static constexpr size_t kWriteWindowData = kMaxPayload - kWriteWindowHeader;
  static constexpr uint8_t kCapabilityWriteWindow = 0x01;
  static constexpr uint8_t kCapabilityReadWindow = 0x02;
  static constexpr uint8_t kCapabilityExtend = 0x04;
  static constexpr uint8_t kWindowStart = 0x01;
  static constexpr uint8_t kWindowEnd = 0x02;

  bool request(uint8_t command, const uint8_t* data, uint16_t length,
               uint32_t timeoutMs, PacketView& response);
  bool pathRequest(uint8_t command, const char* path, uint32_t timeoutMs,
                   PacketView& response);
  bool flushWriteBlock(size_t rawLength);
  bool openFileMode(const char* path, uint8_t mode);
  bool checkBlockAck(const PacketView& response, uint8_t sequence,
                     uint16_t accepted);
  bool readFatWindow(uint32_t rawOffset, size_t wanted, WindowSink sink,
                     void* sinkContext, size_t& received);
  bool buildFatCache(VfsFsInfo& info);
  void rememberFsInfo(const VfsFsInfo& info);
  static bool ingestFatWindow(void* context, const uint8_t* data,
                              size_t length);
  void resetWriteState(bool active);
  void setError(const char* format, ...);
  static void handleUnexpected(void* context, const PacketView& packet);

  UartTransport& transport_;
  char lastError_[64];
  uint8_t writeBuffer_[kBlockSize];
  uint8_t fragment_[kMaxPayload];
  size_t writeCount_;
  uint8_t writeSequence_;
  uint8_t readSequence_;
  uint8_t fatSequence_;
  uint8_t capabilities_;
  uint8_t filexCapabilities_;
  uint8_t lastStatus_;
  uint8_t openMode_;
  uint32_t randomOffset_;
  bool randomOffsetValid_;
  bool writeActive_;
  bool writeFailed_;
  FatAllocationCache fatCache_;
  // Последние прочитанные сведения о томе: геометрия читается один раз, а
  // свободное место поддерживается точным через noteFileResized.
  VfsFsInfo fsInfoCache_;
  bool fsInfoValid_;
};

}  // namespace zifi
