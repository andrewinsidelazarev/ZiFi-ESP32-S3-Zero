#include "zifi/diagnostic_log.hpp"

#include <Arduino.h>
#include <LittleFS.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "zifi/protocol.hpp"

namespace zifi {

namespace {

constexpr char kLogPath[] = "/zifi-ring.log";
constexpr char kPartitionLabel[] = "littlefs";
constexpr char kMountPath[] = "/littlefs";

// Файл всегда занимает ровно 48 КиБ. Новая запись перезаписывает самую
// старую, поэтому даже забытый диагностический журнал не заполнит flash.
constexpr size_t kRecordSize = 192;
constexpr size_t kRecordCount = 256;
constexpr size_t kFileSize = kRecordSize * kRecordCount;
constexpr size_t kTextOffset = 32;
constexpr size_t kTextCapacity = 156;
constexpr size_t kCrcOffset = 188;
constexpr uint16_t kRecordVersion = 1;
constexpr uint8_t kMagic[4] = {'Z', 'L', 'O', 'G'};
constexpr size_t kSnapshotCapacity = 72 * 1024;
// Полный размер постоянного кольца одновременно держим в PSRAM. Иначе один
// длинный Windows-read с 4-КиБ запросами вытеснял именно события, которые
// предшествовали сбою, ещё до остановки SMB и безопасного flush во flash.
constexpr size_t kPsramPendingCount = kRecordCount;
constexpr size_t kInternalPendingCount = 16;

SemaphoreHandle_t logMutex = nullptr;
bool logReady = false;
uint32_t nextSequence = 1;
uint8_t* pendingRecords = nullptr;
size_t pendingCapacity = 0;
size_t pendingHead = 0;
size_t pendingCount = 0;

bool mountLogFs() {
  return LittleFS.begin(true, kMountPath, 3, kPartitionLabel);
}

bool recordValid(const uint8_t record[kRecordSize]) {
  if (memcmp(record, kMagic, sizeof(kMagic)) != 0 ||
      readLe16(record + 4) != kRecordVersion ||
      readLe16(record + 6) > kTextCapacity) {
    return false;
  }
  return crc32IsoHdlc(record, kCrcOffset) == readLe32(record + kCrcOffset);
}

bool readSlot(File& file, size_t slot, uint8_t record[kRecordSize]) {
  return slot < kRecordCount && file.seek(slot * kRecordSize) &&
         file.read(record, kRecordSize) == kRecordSize;
}

bool ensureRingFile() {
  File existing = LittleFS.open(kLogPath, "r");
  const bool correctSize = existing && existing.size() == kFileSize;
  existing.close();
  if (correctSize) {
    return true;
  }

  LittleFS.remove(kLogPath);
  File file = LittleFS.open(kLogPath, "w");
  if (!file) {
    return false;
  }
  uint8_t empty[kRecordSize] = {};
  for (size_t slot = 0; slot < kRecordCount; ++slot) {
    if (file.write(empty, sizeof(empty)) != sizeof(empty)) {
      file.close();
      LittleFS.remove(kLogPath);
      return false;
    }
  }
  file.flush();
  file.close();
  return true;
}

uint32_t findLastSequence(File& file) {
  uint8_t record[kRecordSize];
  uint32_t highest = 0;
  for (size_t slot = 0; slot < kRecordCount; ++slot) {
    if (readSlot(file, slot, record) && recordValid(record)) {
      const uint32_t sequence = readLe32(record + 8);
      if (sequence > highest) {
        highest = sequence;
      }
    }
  }
  return highest;
}

bool takeLogMutex(TickType_t timeout) {
  return logMutex != nullptr && xSemaphoreTake(logMutex, timeout) == pdTRUE;
}

void giveLogMutex() {
  if (logMutex != nullptr) {
    xSemaphoreGive(logMutex);
  }
}

uint8_t* pendingRecord(size_t logicalIndex) {
  if (pendingRecords == nullptr || pendingCapacity == 0 ||
      logicalIndex >= pendingCount) {
    return nullptr;
  }
  const size_t physicalIndex = (pendingHead + logicalIndex) % pendingCapacity;
  return pendingRecords + physicalIndex * kRecordSize;
}

bool allocatePendingRecords() {
  if (pendingRecords != nullptr && pendingCapacity != 0) {
    return true;
  }
  pendingCapacity = kPsramPendingCount;
  pendingRecords = static_cast<uint8_t*>(heap_caps_malloc(
      pendingCapacity * kRecordSize,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (pendingRecords == nullptr) {
    pendingCapacity = kInternalPendingCount;
    pendingRecords = static_cast<uint8_t*>(heap_caps_malloc(
        pendingCapacity * kRecordSize,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (pendingRecords == nullptr) {
    pendingCapacity = 0;
    return false;
  }
  pendingHead = 0;
  pendingCount = 0;
  return true;
}

void enqueueRecord(const uint8_t record[kRecordSize]) {
  if (pendingRecords == nullptr || pendingCapacity == 0) {
    return;
  }
  if (pendingCount == pendingCapacity) {
    pendingHead = (pendingHead + 1) % pendingCapacity;
    --pendingCount;
  }
  const size_t tail = (pendingHead + pendingCount) % pendingCapacity;
  memcpy(pendingRecords + tail * kRecordSize, record, kRecordSize);
  ++pendingCount;
}

// logMutex должен быть захвачен вызывающим. До полного успешного прохода RAM
// не очищаем: короткая запись или ошибка mount безопасно повторятся позже.
bool flushPendingLocked() {
  if (pendingCount == 0) {
    return true;
  }
  if (!mountLogFs()) {
    return false;
  }

  bool ok = false;
  File file = LittleFS.open(kLogPath, "r+");
  if (file) {
    ok = true;
    for (size_t index = 0; index < pendingCount; ++index) {
      const uint8_t* record = pendingRecord(index);
      const uint32_t sequence = readLe32(record + 8);
      const size_t slot = (sequence - 1) % kRecordCount;
      if (!file.seek(slot * kRecordSize) ||
          file.write(record, kRecordSize) != kRecordSize) {
        ok = false;
        break;
      }
    }
    if (ok) {
      file.flush();
    }
    file.close();
  }
  LittleFS.end();
  if (ok) {
    pendingHead = 0;
    pendingCount = 0;
  }
  return ok;
}

}  // namespace

bool diagnosticLogBegin() {
  if (logMutex == nullptr) {
    logMutex = xSemaphoreCreateMutex();
  }
  if (!takeLogMutex(pdMS_TO_TICKS(1000))) {
    return false;
  }

  bool ok = allocatePendingRecords();
  const bool mounted = ok && mountLogFs();
  ok = ok && mounted;
  if (ok) {
    ok = ensureRingFile();
  }
  if (ok) {
    File file = LittleFS.open(kLogPath, "r");
    if (!file) {
      ok = false;
    } else {
      const uint32_t highest = findLastSequence(file);
      nextSequence = highest == UINT32_MAX ? 1 : highest + 1;
      file.close();
    }
  }
  if (mounted) {
    LittleFS.end();
  }
  logReady = ok;
  giveLogMutex();
  return ok;
}

void diagnosticLogEvent(const char* format, ...) {
  if (!logReady || format == nullptr) {
    return;
  }

  char text[kTextCapacity + 1] = {};
  va_list arguments;
  va_start(arguments, format);
  const int formatted = vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  const size_t textLength = formatted <= 0
                                ? 0
                                : strnlen(text, kTextCapacity);

  // Форматирование не держит mutex и не задерживает другой поток, который
  // может как раз пакетно выгружать накопившиеся записи.
  if (!takeLogMutex(0)) {
    return;
  }

  uint8_t record[kRecordSize] = {};
  memcpy(record, kMagic, sizeof(kMagic));
  writeLe16(record + 4, kRecordVersion);
  writeLe16(record + 6, static_cast<uint16_t>(textLength));
  writeLe32(record + 8, nextSequence);
  writeLe32(record + 12, millis());
  writeLe32(record + 16,
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  writeLe32(record + 20, heap_caps_get_minimum_free_size(
                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  writeLe32(record + 24,
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  writeLe32(record + 28, heap_caps_get_minimum_free_size(
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  memcpy(record + kTextOffset, text, textLength);
  writeLe32(record + kCrcOffset, crc32IsoHdlc(record, kCrcOffset));

  enqueueRecord(record);
  ++nextSequence;
  if (nextSequence == 0) {
    nextSequence = 1;
  }
  giveLogMutex();
}

bool diagnosticLogFlush() {
  if (!logReady || !takeLogMutex(0)) {
    return false;
  }
  const bool ok = flushPendingLocked();
  giveLogMutex();
  return ok;
}

bool diagnosticLogSnapshot(DiagnosticLogSnapshot& snapshot) {
  snapshot.data = nullptr;
  snapshot.length = 0;
  if (!logReady || !takeLogMutex(pdMS_TO_TICKS(1000))) {
    return false;
  }

  if (!flushPendingLocked()) {
    giveLogMutex();
    return false;
  }

  uint8_t* output = static_cast<uint8_t*>(heap_caps_malloc(
      kSnapshotCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (output == nullptr) {
    output = static_cast<uint8_t*>(heap_caps_malloc(
        kSnapshotCapacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (output == nullptr || !mountLogFs()) {
    heap_caps_free(output);
    giveLogMutex();
    return false;
  }

  bool ok = false;
  File file = LittleFS.open(kLogPath, "r");
  if (file) {
    const uint32_t highest = findLastSequence(file);
    const uint32_t first = highest > kRecordCount
                               ? highest - static_cast<uint32_t>(kRecordCount) + 1
                               : 1;
    uint8_t record[kRecordSize];
    size_t used = 0;
    ok = true;
    for (uint32_t sequence = first; sequence <= highest; ++sequence) {
      const size_t slot = (sequence - 1) % kRecordCount;
      if (!readSlot(file, slot, record) || !recordValid(record) ||
          readLe32(record + 8) != sequence) {
        continue;
      }
      char line[320];
      const uint16_t textLength = readLe16(record + 6);
      const int length = snprintf(
          line, sizeof(line),
          "seq=%lu ms=%lu ram=%lu ram_min=%lu psram=%lu psram_min=%lu %.*s\n",
          static_cast<unsigned long>(sequence),
          static_cast<unsigned long>(readLe32(record + 12)),
          static_cast<unsigned long>(readLe32(record + 16)),
          static_cast<unsigned long>(readLe32(record + 20)),
          static_cast<unsigned long>(readLe32(record + 24)),
          static_cast<unsigned long>(readLe32(record + 28)),
          static_cast<int>(textLength),
          reinterpret_cast<const char*>(record + kTextOffset));
      if (length <= 0 || static_cast<size_t>(length) >= sizeof(line) ||
          used + static_cast<size_t>(length) > kSnapshotCapacity) {
        ok = false;
        break;
      }
      memcpy(output + used, line, static_cast<size_t>(length));
      used += static_cast<size_t>(length);
    }
    if (ok) {
      snapshot.data = output;
      snapshot.length = used;
      output = nullptr;
    }
    file.close();
  }
  LittleFS.end();
  heap_caps_free(output);
  giveLogMutex();
  return ok;
}

void diagnosticLogFreeSnapshot(DiagnosticLogSnapshot& snapshot) {
  heap_caps_free(snapshot.data);
  snapshot.data = nullptr;
  snapshot.length = 0;
}

}  // namespace zifi

extern "C" void zifi_diagnostic_log_rpc_stage(const char* stage,
                                               int32_t valueA,
                                               int32_t valueB) {
  zifi::diagnosticLogEvent("RPC stage=%s a=%ld b=%ld",
                           stage == nullptr ? "(null)" : stage,
                           static_cast<long>(valueA),
                           static_cast<long>(valueB));
}
