#include "zifi/vfs_bridge.hpp"

#include <esp_heap_caps.h>

#include <stdio.h>
#include <string.h>

namespace zifi {
namespace {

size_t minimum(size_t left, size_t right) {
  return left < right ? left : right;
}

}  // namespace

VfsBridge::VfsBridge(UartTransport& transport)
    : vfs_(transport),
      exchange_(nullptr),
      requestQueue_(nullptr),
      responseQueue_(nullptr),
      networkToVfs_(),
      vfsToNetwork_(),
      networkToVfsStorage_(nullptr),
      vfsToNetworkStorage_(nullptr),
      ready_(false),
      requestPending_(false),
      buffersInPsram_(false),
      writeSession_(false) {}

bool VfsBridge::allocateRings(bool psramAvailable) {
  size_t capacity = kPsramRingCapacity;
  uint32_t capabilities = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  if (psramAvailable) {
    networkToVfsStorage_ = static_cast<uint8_t*>(
        heap_caps_malloc(capacity, capabilities));
    vfsToNetworkStorage_ = static_cast<uint8_t*>(
        heap_caps_malloc(capacity, capabilities));
  }
  if (networkToVfsStorage_ != nullptr && vfsToNetworkStorage_ != nullptr) {
    buffersInPsram_ = true;
  } else {
    if (networkToVfsStorage_ != nullptr) {
      heap_caps_free(networkToVfsStorage_);
    }
    if (vfsToNetworkStorage_ != nullptr) {
      heap_caps_free(vfsToNetworkStorage_);
    }
    networkToVfsStorage_ = nullptr;
    vfsToNetworkStorage_ = nullptr;
    capacity = kFallbackRingCapacity;
    capabilities = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    networkToVfsStorage_ = static_cast<uint8_t*>(
        heap_caps_malloc(capacity, capabilities));
    vfsToNetworkStorage_ = static_cast<uint8_t*>(
        heap_caps_malloc(capacity, capabilities));
    buffersInPsram_ = false;
  }

  if (networkToVfsStorage_ == nullptr || vfsToNetworkStorage_ == nullptr) {
    if (networkToVfsStorage_ != nullptr) {
      heap_caps_free(networkToVfsStorage_);
    }
    if (vfsToNetworkStorage_ != nullptr) {
      heap_caps_free(vfsToNetworkStorage_);
    }
    networkToVfsStorage_ = nullptr;
    vfsToNetworkStorage_ = nullptr;
    return false;
  }
  return networkToVfs_.attach(networkToVfsStorage_, capacity) &&
         vfsToNetwork_.attach(vfsToNetworkStorage_, capacity);
}

bool VfsBridge::begin(bool psramAvailable) {
  if (ready_) {
    return true;
  }
  exchange_ = static_cast<Exchange*>(heap_caps_calloc(
      1, sizeof(Exchange), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  requestQueue_ = xQueueCreate(1, sizeof(uint8_t));
  responseQueue_ = xQueueCreate(1, sizeof(uint8_t));
  if (exchange_ == nullptr || requestQueue_ == nullptr ||
      responseQueue_ == nullptr || !allocateRings(psramAvailable)) {
    return false;
  }
  vfs_.beginFatCache(psramAvailable);
  ready_ = true;
  return true;
}

bool VfsBridge::submit(VfsOperation operation, const char* path,
                       uint32_t value) {
  if (!ready_ || requestPending_) {
    return false;
  }
  exchange_->request.operation = operation;
  exchange_->request.value = value;
  exchange_->request.offset = 0;
  exchange_->request.flags = 0;
  memset(&exchange_->request.metadata, 0, sizeof(exchange_->request.metadata));
  exchange_->request.path[0] = 0;
  exchange_->request.path2[0] = 0;
  if (path != nullptr) {
    const size_t length = strlen(path);
    if (length >= sizeof(exchange_->request.path)) {
      return false;
    }
    memcpy(exchange_->request.path, path, length + 1);
  }
  const uint8_t token = 1;
  if (xQueueSend(requestQueue_, &token, 0) != pdTRUE) {
    return false;
  }
  pendingOperation_ = exchange_->request.operation;
  pendingSinceMs_ = millis();
  requestPending_ = true;
  return true;
}

bool VfsBridge::submitAt(VfsOperation operation, uint32_t offset,
                         uint32_t length) {
  // kNoteResize пользуется теми же двумя числами: offset — прежняя длина
  // файла, value — новая. Это уведомление владельцу счётчика свободного места.
  if (!ready_ || requestPending_ ||
      (operation != VfsOperation::kReadAt &&
       operation != VfsOperation::kWriteAt &&
       operation != VfsOperation::kNoteResize)) {
    return false;
  }
  exchange_->request.operation = operation;
  exchange_->request.value = length;
  exchange_->request.offset = offset;
  exchange_->request.flags = 0;
  exchange_->request.path[0] = 0;
  exchange_->request.path2[0] = 0;
  memset(&exchange_->request.metadata, 0, sizeof(exchange_->request.metadata));
  const uint8_t token = 1;
  if (xQueueSend(requestQueue_, &token, 0) != pdTRUE) {
    return false;
  }
  pendingOperation_ = exchange_->request.operation;
  pendingSinceMs_ = millis();
  requestPending_ = true;
  return true;
}

bool VfsBridge::submitRename(const char* oldPath, const char* newName,
                             bool directory) {
  if (!ready_ || requestPending_ || oldPath == nullptr || newName == nullptr) {
    return false;
  }
  const size_t oldLength = strlen(oldPath);
  const size_t newLength = strlen(newName);
  if (oldLength >= sizeof(exchange_->request.path) ||
      newLength >= sizeof(exchange_->request.path2)) {
    return false;
  }
  exchange_->request.operation = VfsOperation::kRename;
  exchange_->request.value = directory ? 1 : 0;
  exchange_->request.offset = 0;
  exchange_->request.flags = 0;
  memset(&exchange_->request.metadata, 0, sizeof(exchange_->request.metadata));
  memcpy(exchange_->request.path, oldPath, oldLength + 1);
  memcpy(exchange_->request.path2, newName, newLength + 1);
  const uint8_t token = 1;
  if (xQueueSend(requestQueue_, &token, 0) != pdTRUE) {
    return false;
  }
  pendingOperation_ = exchange_->request.operation;
  pendingSinceMs_ = millis();
  requestPending_ = true;
  return true;
}

bool VfsBridge::submitMoveRename(const char* oldPath, const char* newPath,
                                 bool directory, bool replace) {
  if (!ready_ || requestPending_ || oldPath == nullptr || newPath == nullptr) {
    return false;
  }
  const size_t oldLength = strlen(oldPath);
  const size_t newLength = strlen(newPath);
  if (oldLength >= sizeof(exchange_->request.path) ||
      newLength >= sizeof(exchange_->request.path2)) {
    return false;
  }
  exchange_->request.operation = VfsOperation::kMoveRename;
  exchange_->request.value = 0;
  exchange_->request.offset = 0;
  exchange_->request.flags = static_cast<uint8_t>((directory ? 1 : 0) |
                                                   (replace ? 2 : 0));
  memset(&exchange_->request.metadata, 0, sizeof(exchange_->request.metadata));
  memcpy(exchange_->request.path, oldPath, oldLength + 1);
  memcpy(exchange_->request.path2, newPath, newLength + 1);
  const uint8_t token = 1;
  if (xQueueSend(requestQueue_, &token, 0) != pdTRUE) {
    return false;
  }
  pendingOperation_ = exchange_->request.operation;
  pendingSinceMs_ = millis();
  requestPending_ = true;
  return true;
}

bool VfsBridge::submitMetadata(const VfsMetadata& metadata) {
  if (!ready_ || requestPending_) {
    return false;
  }
  exchange_->request.operation = VfsOperation::kSetMetadata;
  exchange_->request.value = 0;
  exchange_->request.offset = 0;
  exchange_->request.flags = 0;
  exchange_->request.path[0] = 0;
  exchange_->request.path2[0] = 0;
  memcpy(&exchange_->request.metadata, &metadata, sizeof(metadata));
  const uint8_t token = 1;
  if (xQueueSend(requestQueue_, &token, 0) != pdTRUE) {
    return false;
  }
  pendingOperation_ = exchange_->request.operation;
  pendingSinceMs_ = millis();
  requestPending_ = true;
  return true;
}

bool VfsBridge::reclaim(uint32_t timeoutMs) {
  if (!ready_ || !requestPending_) {
    return true;
  }
  VfsResult discarded;
  const uint32_t started = millis();
  for (;;) {
    if (takeResult(discarded)) {
      return true;
    }
    if (static_cast<uint32_t>(millis() - started) >= timeoutMs) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

bool VfsBridge::takeResult(VfsResult& result) {
  if (!ready_ || !requestPending_) {
    return false;
  }
  uint8_t token = 0;
  if (xQueueReceive(responseQueue_, &token, 0) != pdTRUE) {
    return false;
  }
  memcpy(&result, &exchange_->result, sizeof(result));
  requestPending_ = false;
  return true;
}

size_t VfsBridge::writeFromNetwork(const uint8_t* data, size_t length) {
  return networkToVfs_.write(data, length);
}

size_t VfsBridge::readForNetwork(uint8_t* output, size_t capacity) {
  return vfsToNetwork_.read(output, capacity);
}

void VfsBridge::finish(bool success, const char* error) {
  exchange_->result.success = success;
  exchange_->result.status = error == nullptr ? vfs_.lastStatus() : 0xFF;
  if (success) {
    snprintf(exchange_->result.error, sizeof(exchange_->result.error), "none");
    return;
  }
  const char* text = error == nullptr ? vfs_.lastError() : error;
  snprintf(exchange_->result.error, sizeof(exchange_->result.error), "%s", text);
}

void VfsBridge::processRead() {
  const size_t room = vfsToNetwork_.freeSpace();
  const size_t maxWindow = vfs_.openMode() == 3
                               ? VfsClient::kFilexTransferWindowSize
                               : VfsClient::kTransferWindowSize;
  const size_t pumpLimit = exchange_->request.operation == VfsOperation::kReadAt
                               ? maxWindow
                               : kMaxPumpPerRequest;
  const size_t wanted = minimum(
      minimum(static_cast<size_t>(exchange_->request.value),
              pumpLimit),
      room);
  if (wanted == 0) {
    exchange_->result.wouldBlock = room == 0;
    finish(true);
    return;
  }
  size_t received = 0;
  if (!vfs_.readFileWindow(wanted, writeNetworkWindow, this, received)) {
    finish(false);
    return;
  }
  exchange_->result.transferred = static_cast<uint32_t>(received);
  exchange_->result.atEnd = received == 0;
  finish(true);
}

void VfsBridge::processWrite() {
  const size_t maxWindow = vfs_.openMode() == 3
                               ? VfsClient::kFilexTransferWindowSize
                               : VfsClient::kTransferWindowSize;
  const size_t pumpLimit = exchange_->request.operation == VfsOperation::kWriteAt
                               ? maxWindow
                               : kMaxPumpPerRequest;
  const size_t requested = minimum(
      static_cast<size_t>(exchange_->request.value),
      networkToVfs_.available());
  if (requested == 0) {
    exchange_->result.wouldBlock = networkToVfs_.available() == 0;
    finish(true);
    return;
  }

  size_t transferred = 0;
  while (transferred < requested) {
    const size_t window = minimum(requested - transferred, pumpLimit);
    if (!vfs_.writeFileWindow(window, readNetworkWindow, this)) {
      exchange_->result.transferred = static_cast<uint32_t>(transferred);
      finish(false);
      return;
    }
    // Читающая позиция публикуется только после ACK соответствующего окна.
    // Уже подтверждённые Z80 окна не переотправляются при более поздней ошибке.
    if (networkToVfs_.discard(window) != window) {
      exchange_->result.transferred = static_cast<uint32_t>(transferred);
      finish(false, "ingress-race");
      return;
    }
    transferred += window;
    exchange_->result.transferred = static_cast<uint32_t>(transferred);
  }
  finish(true);
}

size_t VfsBridge::readNetworkWindow(void* context, size_t offset,
                                    uint8_t* output, size_t capacity) {
  auto* self = static_cast<VfsBridge*>(context);
  return self->networkToVfs_.peekAt(offset, output, capacity);
}

bool VfsBridge::writeNetworkWindow(void* context, const uint8_t* data,
                                   size_t length) {
  auto* self = static_cast<VfsBridge*>(context);
  return self->vfsToNetwork_.write(data, length) == length;
}

void VfsBridge::processCore1() {
  memset(&exchange_->result, 0, sizeof(exchange_->result));
  VfsEntry entry;
  bool atEnd = false;
  switch (exchange_->request.operation) {
    case VfsOperation::kResetBuffers:
      // Инициатор обязан остановить запись и чтение кольца до этого запроса.
      // Отдельная операция позволяет FTP сначала очистить тракт, затем
      // принять данные сокета в PSRAM ещё до медленного VFS_OPEN.
      networkToVfs_.reset();
      vfsToNetwork_.reset();
      finish(true);
      break;
    case VfsOperation::kStat:
      if (vfs_.stat(exchange_->request.path, entry)) {
        exchange_->result.isDirectory = entry.isDirectory;
        exchange_->result.size = entry.size;
        finish(true);
      } else {
        finish(false);
      }
      break;
    case VfsOperation::kOpenDirectory:
      finish(vfs_.openDirectory(exchange_->request.path));
      break;
    case VfsOperation::kReadDirectory:
      if (vfs_.readDirectory(entry, atEnd)) {
        exchange_->result.atEnd = atEnd;
        exchange_->result.isDirectory = entry.isDirectory;
        exchange_->result.size = entry.size;
        if (!atEnd) {
          snprintf(exchange_->result.name, sizeof(exchange_->result.name),
                   "%s", entry.name);
        }
        finish(true);
      } else {
        finish(false);
      }
      break;
    case VfsOperation::kOpenRead:
      if (vfs_.openFile(exchange_->request.path, false)) {
        writeSession_ = false;
        finish(true);
      } else {
        finish(false);
      }
      break;
    case VfsOperation::kOpenWrite: {
      const bool opened = vfs_.openFile(exchange_->request.path, true);
      writeSession_ = opened;
      finish(opened);
      break;
    }
    case VfsOperation::kOpenAppend: {
      const bool opened = vfs_.openFileAppend(exchange_->request.path);
      writeSession_ = opened;
      finish(opened);
      break;
    }
    case VfsOperation::kOpenRandom: {
      const bool opened = vfs_.openFileRandom(exchange_->request.path);
      writeSession_ = opened;
      finish(opened);
      break;
    }
    case VfsOperation::kRead:
      processRead();
      break;
    case VfsOperation::kWrite:
      processWrite();
      break;
    case VfsOperation::kReadAt:
      if (vfs_.openMode() == 3) {
        if (vfs_.seekFile(exchange_->request.offset)) {
          processRead();
        } else {
          finish(false);
        }
      } else {
        processRead();
      }
      break;
    case VfsOperation::kWriteAt:
      if (vfs_.openMode() == 3) {
        if (vfs_.seekFile(exchange_->request.offset)) {
          processWrite();
        } else {
          finish(false);
        }
      } else {
        processWrite();
      }
      break;
    case VfsOperation::kExtend:
      if (vfs_.extendFile(exchange_->request.value)) {
        exchange_->result.transferred = exchange_->request.value;
        finish(true);
      } else {
        finish(false);
      }
      break;
    case VfsOperation::kSetEof: {
      uint32_t actual = 0;
      if (vfs_.setFileSize(exchange_->request.value, actual)) {
        exchange_->result.size = actual;
        finish(true);
      } else {
        finish(false);
      }
      break;
    }
    case VfsOperation::kGetFsInfo:
      // value: 0 — обычный запрос, 1 — разрешено достроить карту FAT,
      // 2 — только готовый ответ, без обращения к Z80.
      if (vfs_.getFsInfo(exchange_->result.fsInfo,
                         exchange_->request.value == 1,
                         exchange_->request.value == 2)) {
        finish(true);
      } else {
        finish(false);
      }
      break;
    case VfsOperation::kNoteResize:
      // Верхний слой знает прежнюю и новую длину файла, владелец счётчика —
      // нижний. Это единственный обмен между ними.
      vfs_.noteFileResized(exchange_->request.offset, exchange_->request.value);
      finish(true);
      break;
    case VfsOperation::kInvalidateFsInfo:
      vfs_.invalidateFsCache();
      finish(true);
      break;
    case VfsOperation::kCloseCommit:
      if (writeSession_ && networkToVfs_.available() != 0) {
        finish(false, "ingress-pending");
      } else {
        const bool closed = vfs_.closeFile(true);
        writeSession_ = false;
        finish(closed);
      }
      break;
    case VfsOperation::kCloseAbort: {
      const bool closed = vfs_.closeFile(false);
      writeSession_ = false;
      networkToVfs_.reset();
      finish(closed);
      break;
    }
    case VfsOperation::kDelete:
      finish(vfs_.remove(exchange_->request.path));
      break;
    case VfsOperation::kMkdir:
      finish(vfs_.makeDirectory(exchange_->request.path));
      break;
    case VfsOperation::kRename:
      finish(vfs_.rename(exchange_->request.path, exchange_->request.path2,
                         exchange_->request.value != 0));
      break;
    case VfsOperation::kMoveRename:
      finish(vfs_.moveRename(exchange_->request.path,
                             exchange_->request.path2,
                             (exchange_->request.flags & 1) != 0,
                             (exchange_->request.flags & 2) != 0));
      break;
    case VfsOperation::kSetMetadata:
      if (vfs_.setMetadata(exchange_->request.metadata,
                           exchange_->result.appliedAttributes)) {
        finish(true);
      } else {
        finish(false);
      }
      break;
  }
}

void VfsBridge::pollCore1() {
  if (!ready_) {
    return;
  }
  uint8_t token = 0;
  if (xQueueReceive(requestQueue_, &token, 0) != pdTRUE) {
    return;
  }
  processCore1();
  xQueueOverwrite(responseQueue_, &token);
}

}  // namespace zifi
