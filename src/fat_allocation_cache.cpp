#include "zifi/fat_allocation_cache.hpp"

#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace zifi {

FatAllocationCache::FatAllocationCache()
    : bits_(nullptr),
      bytes_(0),
      totalClusters_(0),
      fatSectors_(0),
      serial_(0),
      rawOffset_(0),
      freeClusters_(0),
      activeFat_(0),
      enabled_(false),
      building_(false),
      valid_(false),
      exactMap_(false) {}

FatAllocationCache::~FatAllocationCache() { clear(); }

bool FatAllocationCache::begin(bool psramAvailable) {
  clear();
  enabled_ = psramAvailable;
  return enabled_;
}

void* FatAllocationCache::allocate(size_t bytes) {
  if (!enabled_ || bytes == 0 || bytes > kMaximumBytes) {
    return nullptr;
  }
#if defined(ESP_PLATFORM)
  return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return malloc(bytes);
#endif
}

void FatAllocationCache::release() {
  if (bits_ == nullptr) {
    return;
  }
#if defined(ESP_PLATFORM)
  heap_caps_free(bits_);
#else
  free(bits_);
#endif
  bits_ = nullptr;
  bytes_ = 0;
}

void FatAllocationCache::clear() {
  release();
  totalClusters_ = 0;
  fatSectors_ = 0;
  serial_ = 0;
  rawOffset_ = 0;
  freeClusters_ = 0;
  activeFat_ = 0;
  building_ = false;
  valid_ = false;
  exactMap_ = false;
  enabled_ = false;
}

void FatAllocationCache::invalidate() {
  building_ = false;
  valid_ = false;
  exactMap_ = false;
  rawOffset_ = 0;
  freeClusters_ = 0;
}

bool FatAllocationCache::prepare(uint32_t totalClusters,
                                 uint32_t fatSectors, uint32_t serial,
                                 uint8_t activeFat) {
  if (!enabled_ || totalClusters == 0 || fatSectors == 0) {
    return false;
  }
  const uint64_t requiredRawBytes =
      (static_cast<uint64_t>(totalClusters) + 2U) * 4U;
  const uint64_t availableRawBytes = static_cast<uint64_t>(fatSectors) * 512U;
  if (requiredRawBytes > availableRawBytes) {
    return false;
  }
  const size_t wantedBytes =
      static_cast<size_t>((static_cast<uint64_t>(totalClusters) + 7U) / 8U);
  if (wantedBytes == 0 || wantedBytes > kMaximumBytes) {
    return false;
  }
  if (bits_ == nullptr || bytes_ != wantedBytes) {
    release();
    bits_ = static_cast<uint8_t*>(allocate(wantedBytes));
    if (bits_ == nullptr) {
      return false;
    }
    bytes_ = wantedBytes;
  }
  memset(bits_, 0, bytes_);
  totalClusters_ = totalClusters;
  fatSectors_ = fatSectors;
  serial_ = serial;
  activeFat_ = activeFat;
  rawOffset_ = 0;
  freeClusters_ = 0;
  building_ = true;
  valid_ = false;
  return true;
}

bool FatAllocationCache::ingestMap(uint32_t rawOffset, const uint8_t* data,
                                   size_t length) {
  if (!building_ || data == nullptr || length == 0 ||
      (rawOffset & 31U) != 0 || rawOffset != rawOffset_ ||
      length > 0xFFFFFFFFUL / 32U) {
    invalidate();
    return false;
  }
  const uint64_t nextOffset =
      static_cast<uint64_t>(rawOffset) + static_cast<uint64_t>(length) * 32U;
  if (nextOffset > static_cast<uint64_t>(fatSectors_) * 512U) {
    invalidate();
    return false;
  }
  uint32_t entry = rawOffset / 4U;
  for (size_t offset = 0; offset < length; ++offset) {
    for (uint8_t bit = 0; bit < 8; ++bit, ++entry) {
      if (entry < 2 || entry >= totalClusters_ + 2U) {
        continue;
      }
      const uint32_t clusterIndex = entry - 2U;
      if ((data[offset] & static_cast<uint8_t>(1U << bit)) == 0) {
        ++freeClusters_;
      } else {
        bits_[clusterIndex >> 3] |=
            static_cast<uint8_t>(1U << (clusterIndex & 7U));
      }
    }
  }
  rawOffset_ = static_cast<uint32_t>(nextOffset);
  return true;
}

bool FatAllocationCache::finish() {
  const uint64_t requiredRawBytes =
      (static_cast<uint64_t>(totalClusters_) + 2U) * 4U;
  if (!building_ || rawOffset_ < requiredRawBytes ||
      freeClusters_ > totalClusters_) {
    invalidate();
    return false;
  }
  building_ = false;
  valid_ = true;
  exactMap_ = true;
  return true;
}

void FatAllocationCache::noteResize(uint32_t oldSize, uint32_t newSize,
                                    uint32_t bytesPerCluster) {
  if (!valid_ || building_ || bytesPerCluster == 0 || oldSize == newSize) {
    return;
  }
  const uint32_t before =
      static_cast<uint32_t>((static_cast<uint64_t>(oldSize) +
                             bytesPerCluster - 1U) / bytesPerCluster);
  const uint32_t after =
      static_cast<uint32_t>((static_cast<uint64_t>(newSize) +
                             bytesPerCluster - 1U) / bytesPerCluster);
  if (after == before) {
    return;
  }
  // Номера конкретных кластеров сюда не приходят, поэтому биты карты с этого
  // момента приблизительны. Счётчик при этом остаётся точным — именно его
  // спрашивает SMB, а allocated() в прошивке не используется.
  exactMap_ = false;
  if (after > before) {
    const uint32_t taken = after - before;
    freeClusters_ = freeClusters_ > taken ? freeClusters_ - taken : 0;
    return;
  }
  const uint32_t released = before - after;
  const uint32_t free = freeClusters_ + released;
  // Защита от переполнения и от возврата большего, чем есть на томе.
  freeClusters_ =
      free < freeClusters_ || free > totalClusters_ ? totalClusters_ : free;
}

bool FatAllocationCache::matches(uint32_t totalClusters,
                                 uint32_t fatSectors, uint32_t serial,
                                 uint8_t activeFat) const {
  return valid_ && totalClusters_ == totalClusters &&
         fatSectors_ == fatSectors && serial_ == serial &&
         activeFat_ == activeFat;
}

bool FatAllocationCache::allocated(uint32_t cluster) const {
  if (!valid_ || cluster < 2 || cluster >= totalClusters_ + 2U) {
    return false;
  }
  const uint32_t index = cluster - 2U;
  return (bits_[index >> 3] & static_cast<uint8_t>(1U << (index & 7U))) != 0;
}

}  // namespace zifi
