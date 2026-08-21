#pragma once

#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "zifi/spsc_ring.hpp"
#include "zifi/vfs_client.hpp"

namespace zifi {

enum class VfsOperation : uint8_t {
  kResetBuffers,
  kStat,
  kOpenDirectory,
  kReadDirectory,
  kOpenRead,
  kOpenWrite,
  kOpenAppend,
  kOpenRandom,
  kRead,
  kWrite,
  kReadAt,
  kWriteAt,
  kExtend,
  kSetEof,
  kGetFsInfo,
  // Учёт свободного места: изменение длины файла и полный сброс сведений о
  // томе. Счётчик живёт в VfsClient, дельту сообщает вызывающий.
  kNoteResize,
  kInvalidateFsInfo,
  kCloseCommit,
  kCloseAbort,
  kDelete,
  kMkdir,
  kRename,
  kMoveRename,
  kSetMetadata,
};

struct VfsResult {
  bool success = false;
  bool atEnd = false;
  bool wouldBlock = false;
  bool isDirectory = false;
  uint8_t status = 0;
  uint8_t appliedAttributes = 0;
  uint32_t size = 0;
  uint32_t transferred = 0;
  char name[256] = {};
  char error[64] = {};
  VfsFsInfo fsInfo;
};

// Асинхронная граница между сетевым ядром 0 и ядром 1 с UART/VFS.
// Методы submit/takeResult и доступ к сетевым сторонам колец принадлежат
// ядру 0. pollCore1 — единственная точка исполнения VfsClient и UART.
class VfsBridge {
 public:
  static constexpr size_t kPsramRingCapacity = 64 * 1024;
  static constexpr size_t kFallbackRingCapacity = 4 * 1024;
  static constexpr size_t kMaxPumpPerRequest = VfsClient::kTransferWindowSize;

  explicit VfsBridge(UartTransport& transport);

  bool begin(bool psramAvailable);
  bool ready() const { return ready_; }

  // Интерфейс ядра 0. Допускается ровно один управляющий VFS-запрос.
  bool submit(VfsOperation operation, const char* path = nullptr,
              uint32_t value = 0);
  bool submitAt(VfsOperation operation, uint32_t offset, uint32_t length);
  bool submitRename(const char* oldPath, const char* newName,
                    bool directory);
  bool submitMoveRename(const char* oldPath, const char* newPath,
                        bool directory, bool replace);
  bool submitMetadata(const VfsMetadata& metadata);
  bool takeResult(VfsResult& result);
  // Дождаться результата брошенного обмена и забрать его жетон. Пока core 1
  // не вернул результат, общий Exchange трогать нельзя: принудительный сброс
  // флага позволил бы следующему запросу перезаписать ещё используемую память.
  // Возвращает false, оставляя мост занятым и пригодным для следующей попытки.
  bool reclaim(uint32_t timeoutMs);
  bool requestPending() const { return requestPending_; }

  // Сеть -> VFS (STOR/PUT) и VFS -> сеть (RETR/GET).
  size_t writeFromNetwork(const uint8_t* data, size_t length);
  size_t readForNetwork(uint8_t* output, size_t capacity);
  size_t networkToVfsAvailable() const { return networkToVfs_.available(); }
  size_t vfsToNetworkAvailable() const { return vfsToNetwork_.available(); }
  size_t networkToVfsFree() const { return networkToVfs_.freeSpace(); }
  size_t vfsToNetworkFree() const { return vfsToNetwork_.freeSpace(); }

  // Интерфейс ядра 1. Вызывать часто из loopTask до обычного опроса UART.
  void pollCore1();

  bool buffersInPsram() const { return buffersInPsram_; }
  size_t ringCapacity() const { return networkToVfs_.capacity(); }

  // Кто именно держит мост и сколько уже держит. Нужно для одной-единственной
  // строки журнала: когда очередной запрос получает отказ «bridge-busy»,
  // виновник должен назваться сам, а не вычисляться перебором версий.
  uint8_t pendingOperation() const {
    return static_cast<uint8_t>(pendingOperation_);
  }
  uint32_t pendingSinceMs() const { return pendingSinceMs_; }
  size_t networkToVfsHighWater() const {
    return networkToVfs_.highWaterMark();
  }
  size_t vfsToNetworkHighWater() const {
    return vfsToNetwork_.highWaterMark();
  }

 private:
  struct Request {
    VfsOperation operation;
    uint32_t value;
    uint32_t offset;
    uint8_t flags;
    VfsMetadata metadata;
    char path[256];
    char path2[256];
  };

  struct Exchange {
    Request request;
    VfsResult result;
  };

  bool allocateRings(bool psramAvailable);
  void processCore1();
  void processRead();
  void processWrite();
  void finish(bool success, const char* error = nullptr);
  static size_t readNetworkWindow(void* context, size_t offset,
                                  uint8_t* output, size_t capacity);
  static bool writeNetworkWindow(void* context, const uint8_t* data,
                                 size_t length);

  VfsClient vfs_;
  Exchange* exchange_;
  QueueHandle_t requestQueue_;
  QueueHandle_t responseQueue_;
  SpscByteRing networkToVfs_;
  SpscByteRing vfsToNetwork_;
  uint8_t* networkToVfsStorage_;
  uint8_t* vfsToNetworkStorage_;
  bool ready_;
  bool requestPending_;
  // Операция незавершённого обмена и время его постановки.
  VfsOperation pendingOperation_ = VfsOperation::kStat;
  uint32_t pendingSinceMs_ = 0;
  bool buffersInPsram_;
  bool writeSession_;
};

}  // namespace zifi
