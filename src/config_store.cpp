#include "zifi/config_store.hpp"

#include <LittleFS.h>

#include <stdio.h>
#include <string.h>

#include "zifi/protocol.hpp"

namespace zifi {
namespace {

constexpr char kConfigPath[] = "/zifi.cfg";
constexpr char kNewPath[] = "/zifi.new";
constexpr char kBackupPath[] = "/zifi.bak";
constexpr char kPartitionLabel[] = "littlefs";
constexpr char kMountPath[] = "/littlefs";
constexpr uint8_t kMagic[4] = {'Z', 'C', 'F', 'G'};
constexpr uint16_t kFormatVersion = 1;
constexpr size_t kHeaderSize = 12;

void setError(char* error, size_t errorSize, const char* text) {
  if (error != nullptr && errorSize != 0) {
    snprintf(error, errorSize, "%s", text);
  }
}

}  // namespace

ConfigStore::ConfigStore()
    : available_(false), valid_(false), checksum_(0), length_(0) {}

uint32_t ConfigStore::crc32(const uint8_t* data, size_t length) {
  // CRC служит детектором изменения, а не криптографической защитой.
  return crc32IsoHdlc(data, length);
}

bool ConfigStore::mount() {
  return LittleFS.begin(false, kMountPath, 3, kPartitionLabel);
}

bool ConfigStore::begin(char* error, size_t errorSize) {
  bool mounted = mount();
  if (!mounted) {
    // Форматируется только именованный раздел 0x3D0000..0x3FFFFF. Оба OTA
    // слота и factory-приложение эта операция не затрагивает.
    if (!LittleFS.format()) {
      setError(error, errorSize, "config fs format");
      return false;
    }
    mounted = mount();
  }
  if (!mounted) {
    setError(error, errorSize, "config fs mount");
    return false;
  }
  LittleFS.end();
  available_ = true;
  setError(error, errorSize, "");
  return true;
}

bool ConfigStore::loadPath(const char* path, uint8_t* output, size_t capacity,
                           size_t& length, uint32_t& checksum) {
  File file = LittleFS.open(path, "r");
  if (!file) {
    return false;
  }
  uint8_t header[kHeaderSize];
  const size_t headerRead = file.read(header, sizeof(header));
  if (headerRead != sizeof(header) ||
      memcmp(header, kMagic, sizeof(kMagic)) != 0 ||
      readLe16(header + 4) != kFormatVersion) {
    file.close();
    return false;
  }
  const uint16_t storedLength = readLe16(header + 6);
  const uint32_t storedChecksum = readLe32(header + 8);
  if (storedLength > kMaxIniSize || storedLength > capacity ||
      file.size() != kHeaderSize + storedLength) {
    file.close();
    return false;
  }
  const size_t dataRead = file.read(output, storedLength);
  file.close();
  if (dataRead != storedLength || crc32(output, storedLength) != storedChecksum) {
    return false;
  }
  length = storedLength;
  checksum = storedChecksum;
  return true;
}

bool ConfigStore::load(uint8_t* output, size_t capacity, size_t& length,
                       char* error, size_t errorSize) {
  length = 0;
  valid_ = false;
  if (!available_ || output == nullptr || capacity < kMaxIniSize) {
    setError(error, errorSize, "config store unavailable");
    return false;
  }
  if (!mount()) {
    setError(error, errorSize, "config fs remount");
    return false;
  }

  uint32_t checksum = 0;
  // При пропадании питания между rename остаётся основной, backup или new.
  // Каждый кандидат проверяется по длине и CRC до разбора как INI.
  const char* candidates[] = {kConfigPath, kBackupPath, kNewPath};
  bool found = false;
  for (const char* path : candidates) {
    if (loadPath(path, output, capacity, length, checksum)) {
      found = true;
      break;
    }
  }
  LittleFS.end();
  if (!found) {
    setError(error, errorSize, "no saved config");
    return false;
  }
  checksum_ = checksum;
  length_ = static_cast<uint16_t>(length);
  valid_ = true;
  setError(error, errorSize, "");
  return true;
}

bool ConfigStore::writeNewFile(const uint8_t* data, size_t length,
                               uint32_t checksum,
                               char* error, size_t errorSize) {
  LittleFS.remove(kNewPath);
  File file = LittleFS.open(kNewPath, "w");
  if (!file) {
    setError(error, errorSize, "config open new");
    return false;
  }
  uint8_t header[kHeaderSize];
  memcpy(header, kMagic, sizeof(kMagic));
  writeLe16(header + 4, kFormatVersion);
  writeLe16(header + 6, static_cast<uint16_t>(length));
  writeLe32(header + 8, checksum);
  const bool written = file.write(header, sizeof(header)) == sizeof(header) &&
                       file.write(data, length) == length;
  file.flush();
  file.close();
  if (!written) {
    LittleFS.remove(kNewPath);
    setError(error, errorSize, "config short write");
    return false;
  }

  // Двухступенчатая замена переживает reset в любой точке.
  LittleFS.remove(kBackupPath);
  const bool hadOld = LittleFS.exists(kConfigPath);
  if (hadOld && !LittleFS.rename(kConfigPath, kBackupPath)) {
    LittleFS.remove(kNewPath);
    setError(error, errorSize, "config backup rename");
    return false;
  }
  if (!LittleFS.rename(kNewPath, kConfigPath)) {
    if (hadOld) {
      LittleFS.rename(kBackupPath, kConfigPath);
    }
    setError(error, errorSize, "config commit rename");
    return false;
  }
  LittleFS.remove(kBackupPath);
  setError(error, errorSize, "");
  return true;
}

ConfigSaveResult ConfigStore::saveIfChanged(const uint8_t* data, size_t length,
                                            char* error, size_t errorSize) {
  if (!available_ || data == nullptr || length == 0 || length > kMaxIniSize) {
    setError(error, errorSize, "config save range");
    return ConfigSaveResult::kError;
  }
  const uint32_t nextChecksum = crc32(data, length);
  if (valid_ && checksum_ == nextChecksum && length_ == length) {
    // Одинаковый zifi.ini не открывает flash на запись.
    setError(error, errorSize, "");
    return ConfigSaveResult::kUnchanged;
  }
  if (!mount()) {
    setError(error, errorSize, "config fs remount");
    return ConfigSaveResult::kError;
  }
  const bool written = writeNewFile(data, length, nextChecksum,
                                    error, errorSize);
  LittleFS.end();
  if (!written) {
    return ConfigSaveResult::kError;
  }
  checksum_ = nextChecksum;
  length_ = static_cast<uint16_t>(length);
  valid_ = true;
  return ConfigSaveResult::kWritten;
}

}  // namespace zifi
