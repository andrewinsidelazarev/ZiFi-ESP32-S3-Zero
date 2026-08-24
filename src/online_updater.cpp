#include "zifi/online_updater.hpp"

#include <Arduino.h>
#include <Update.h>
#include <mbedtls/sha256.h>

#include <stdio.h>
#include <string.h>

#include "zifi/ota_image.hpp"

namespace zifi {
namespace {

constexpr char kGithubHost[] = "raw.githubusercontent.com";
constexpr char kManifestPath[] =
    "/andrewinsidelazarev/ZiFi-ESP32-S3-Zero/refs/heads/main/firmware/"
    "firmware.sha256";
constexpr char kFirmwarePath[] =
    "/andrewinsidelazarev/ZiFi-ESP32-S3-Zero/refs/heads/main/firmware/"
    "firmware.bin";
constexpr uint16_t kHttpsPort = 443;

void setError(char* error, size_t errorSize, const char* text) {
  if (error != nullptr && errorSize != 0) {
    snprintf(error, errorSize, "%s", text != nullptr ? text : "error");
  }
}

void setHttpError(char* error, size_t errorSize, const char* prefix,
                  uint16_t status) {
  if (error != nullptr && errorSize != 0) {
    snprintf(error, errorSize, "%s http %u", prefix,
             static_cast<unsigned>(status));
  }
}

int hexValue(uint8_t value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

bool filenameMatches(const uint8_t* text, size_t length) {
  constexpr char kName[] = "firmware.bin";
  return length == sizeof(kName) - 1 &&
         memcmp(text, kName, sizeof(kName) - 1) == 0;
}

bool versionCharacterAllowed(uint8_t value) {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '.' || value == '_' ||
         value == '+' || value == '-';
}

// VERSION содержит показываемую пользователю версию. Строки SHA сохраняют
// обычный формат sha256sum: HASH, пробелы, необязательный '*', имя.
bool parseManifest(const uint8_t* data, size_t length,
                   OnlineUpdateManifest& manifest) {
  bool digestFound = false;
  bool versionFound = false;
  memset(&manifest, 0, sizeof(manifest));
  size_t line = 0;
  while (line < length) {
    size_t end = line;
    while (end < length && data[end] != '\r' && data[end] != '\n') {
      ++end;
    }
    size_t cursor = line;
    while (cursor < end && (data[cursor] == ' ' || data[cursor] == '\t')) {
      ++cursor;
    }
    constexpr char kVersionTag[] = "VERSION";
    if (end - cursor > sizeof(kVersionTag) - 1 &&
        memcmp(data + cursor, kVersionTag, sizeof(kVersionTag) - 1) == 0 &&
        (data[cursor + sizeof(kVersionTag) - 1] == ' ' ||
         data[cursor + sizeof(kVersionTag) - 1] == '\t')) {
      if (versionFound) {
        return false;
      }
      cursor += sizeof(kVersionTag) - 1;
      while (cursor < end &&
             (data[cursor] == ' ' || data[cursor] == '\t')) {
        ++cursor;
      }
      size_t versionEnd = end;
      while (versionEnd > cursor &&
             (data[versionEnd - 1] == ' ' || data[versionEnd - 1] == '\t')) {
        --versionEnd;
      }
      const size_t versionLength = versionEnd - cursor;
      if (versionLength == 0 ||
          versionLength >= kOnlineUpdateVersionCapacity) {
        return false;
      }
      for (size_t index = 0; index < versionLength; ++index) {
        if (!versionCharacterAllowed(data[cursor + index])) {
          return false;
        }
      }
      memcpy(manifest.version, data + cursor, versionLength);
      manifest.version[versionLength] = 0;
      versionFound = true;
    } else if (end - cursor >= 64) {
      uint8_t parsed[32];
      bool valid = true;
      for (size_t index = 0; index < sizeof(parsed); ++index) {
        const int high = hexValue(data[cursor + index * 2]);
        const int low = hexValue(data[cursor + index * 2 + 1]);
        if (high < 0 || low < 0) {
          valid = false;
          break;
        }
        parsed[index] = static_cast<uint8_t>((high << 4) | low);
      }
      cursor += 64;
      if (valid && cursor < end &&
          (data[cursor] == ' ' || data[cursor] == '\t')) {
        while (cursor < end &&
               (data[cursor] == ' ' || data[cursor] == '\t')) {
          ++cursor;
        }
        if (cursor < end && data[cursor] == '*') {
          ++cursor;
        }
        size_t nameEnd = cursor;
        while (nameEnd < end && data[nameEnd] != ' ' &&
               data[nameEnd] != '\t') {
          ++nameEnd;
        }
        if (filenameMatches(data + cursor, nameEnd - cursor)) {
          if (digestFound) {
            return false;
          }
          memcpy(manifest.sha256, parsed, sizeof(parsed));
          digestFound = true;
        }
      }
    }
    line = end;
    while (line < length && (data[line] == '\r' || data[line] == '\n')) {
      ++line;
    }
  }
  return digestFound && versionFound;
}

bool notify(OnlineUpdateProgress progress, void* context, uint8_t stage,
            uint8_t percent) {
  return progress == nullptr || progress(context, stage, percent);
}

bool finishSha(mbedtls_sha256_context& context, uint8_t digest[32]) {
  const bool ok = mbedtls_sha256_finish_ret(&context, digest) == 0;
  mbedtls_sha256_free(&context);
  return ok;
}

}  // namespace

OnlineUpdater::OnlineUpdater(NetClient& client) : client_(client) {}

bool OnlineUpdater::loadManifest(uint8_t* scratch, size_t scratchSize,
                                 OnlineUpdateManifest& manifest,
                                 OnlineUpdateProgress progress,
                                 void* progressContext, char* error,
                                 size_t errorSize) {
  notify(progress, progressContext, kOnlineUpdateManifest, 0);
  uint16_t status = 0;
  uint32_t contentLength = 0;
  if (!client_.httpGet(kGithubHost, kHttpsPort, kManifestPath, status,
                       contentLength, error, errorSize)) {
    return false;
  }
  if (status < 200 || status >= 300) {
    client_.close();
    setHttpError(error, errorSize, "manifest", status);
    return false;
  }
  if (contentLength == 0 || contentLength >= scratchSize) {
    client_.close();
    setError(error, errorSize, "manifest length");
    return false;
  }

  size_t total = 0;
  bool eof = false;
  while (total < contentLength) {
    size_t received = 0;
    if (!client_.receive(scratch + total, scratchSize - total - 1, received,
                         eof, error, errorSize)) {
      client_.close();
      return false;
    }
    total += received;
    if (total > contentLength || total >= scratchSize) {
      client_.close();
      setError(error, errorSize, "manifest overflow");
      return false;
    }
    if (eof && total < contentLength) {
      client_.close();
      setError(error, errorSize, "manifest short");
      return false;
    }
    if (received == 0) {
      delay(2);
    }
  }
  client_.close();
  scratch[total] = 0;
  if (total != contentLength) {
    setError(error, errorSize, "manifest short");
    return false;
  }
  if (!parseManifest(scratch, total, manifest)) {
    setError(error, errorSize, "manifest version or sha");
    return false;
  }
  notify(progress, progressContext, kOnlineUpdateManifest, 100);
  return true;
}

bool OnlineUpdater::check(uint8_t* scratch, size_t scratchSize,
                          OnlineUpdateManifest& manifest,
                          OnlineUpdateProgress progress,
                          void* progressContext, char* error,
                          size_t errorSize) {
  if (scratch == nullptr || scratchSize < 128) {
    setError(error, errorSize, "check arguments");
    return false;
  }
  return loadManifest(scratch, scratchSize, manifest, progress,
                      progressContext, error, errorSize);
}

bool OnlineUpdater::installFirmware(uint8_t* scratch, size_t scratchSize,
                                    const uint8_t expectedSha[32],
                                    uint8_t digest[32],
                                    OnlineUpdateProgress progress,
                                    void* progressContext, char* error,
                                    size_t errorSize) {
  notify(progress, progressContext, kOnlineUpdateFirmware, 0);
  uint16_t status = 0;
  uint32_t contentLength = 0;
  if (!client_.httpGet(kGithubHost, kHttpsPort, kFirmwarePath, status,
                       contentLength, error, errorSize)) {
    return false;
  }
  if (status < 200 || status >= 300) {
    client_.close();
    setHttpError(error, errorSize, "firmware", status);
    return false;
  }
  if (contentLength < kOtaImageHeaderSize ||
      contentLength > ESP.getFreeSketchSpace()) {
    client_.close();
    setError(error, errorSize, "firmware does not fit");
    return false;
  }
  if (scratch == nullptr || scratchSize < kOtaImageHeaderSize) {
    client_.close();
    setError(error, errorSize, "update buffer");
    return false;
  }

  size_t headerLength = 0;
  bool eof = false;
  while (headerLength < kOtaImageHeaderSize) {
    size_t received = 0;
    if (!client_.receive(scratch + headerLength,
                         kOtaImageHeaderSize - headerLength, received, eof,
                         error, errorSize)) {
      client_.close();
      return false;
    }
    headerLength += received;
    if (eof || (received == 0 && headerLength < kOtaImageHeaderSize)) {
      if (!eof) {
        delay(2);
        continue;
      }
      setError(error, errorSize, "firmware short header");
      return false;
    }
  }
  // До проверки application-заголовка flash вообще не открывается на запись.
  if (!isEsp32S3ApplicationImage(scratch, headerLength)) {
    client_.close();
    setError(error, errorSize, "not ESP32-S3 application");
    return false;
  }

  mbedtls_sha256_context shaContext;
  mbedtls_sha256_init(&shaContext);
  if (mbedtls_sha256_starts_ret(&shaContext, 0) != 0) {
    mbedtls_sha256_free(&shaContext);
    client_.close();
    setError(error, errorSize, "sha256 init");
    return false;
  }
  if (!Update.begin(contentLength, U_FLASH)) {
    mbedtls_sha256_free(&shaContext);
    client_.close();
    setError(error, errorSize, "update begin");
    return false;
  }
  uint32_t total = static_cast<uint32_t>(headerLength);
  if (Update.write(scratch, headerLength) != headerLength ||
      mbedtls_sha256_update_ret(&shaContext, scratch, headerLength) != 0) {
    Update.abort();
    mbedtls_sha256_free(&shaContext);
    client_.close();
    setError(error, errorSize, "first flash block");
    return false;
  }

  uint8_t lastPercent = 0;
  while (total < contentLength) {
    size_t received = 0;
    eof = false;
    const size_t remaining = static_cast<size_t>(contentLength - total);
    const size_t wanted = remaining < scratchSize ? remaining : scratchSize;
    if (!client_.receive(scratch, wanted, received, eof, error, errorSize)) {
      Update.abort();
      mbedtls_sha256_free(&shaContext);
      client_.close();
      return false;
    }
    if (received != 0) {
      if (Update.write(scratch, received) != received ||
          mbedtls_sha256_update_ret(&shaContext, scratch, received) != 0) {
        Update.abort();
        mbedtls_sha256_free(&shaContext);
        client_.close();
        setError(error, errorSize, "flash write");
        return false;
      }
      total += static_cast<uint32_t>(received);
      const uint8_t percent = static_cast<uint8_t>(
          (static_cast<uint64_t>(total) * 100U) / contentLength);
      if (percent == 100 || percent >= static_cast<uint8_t>(lastPercent + 5)) {
        lastPercent = percent;
        notify(progress, progressContext, kOnlineUpdateFirmware, percent);
      }
    } else if (eof) {
      Update.abort();
      mbedtls_sha256_free(&shaContext);
      setError(error, errorSize, "firmware short body");
      return false;
    } else {
      delay(2);
    }
  }
  client_.close();

  notify(progress, progressContext, kOnlineUpdateVerify, 0);
  if (!finishSha(shaContext, digest)) {
    Update.abort();
    setError(error, errorSize, "sha256 finish");
    return false;
  }
  uint8_t difference = 0;
  for (size_t index = 0; index < 32; ++index) {
    difference |= digest[index] ^ expectedSha[index];
  }
  if (difference != 0) {
    Update.abort();
    setError(error, errorSize, "sha256 mismatch");
    return false;
  }
  if (!Update.end(false)) {
    Update.abort();
    setError(error, errorSize, "update end");
    return false;
  }
  notify(progress, progressContext, kOnlineUpdateVerify, 100);
  setError(error, errorSize, "");
  return true;
}

bool OnlineUpdater::install(uint8_t* scratch, size_t scratchSize,
                            uint8_t digest[32],
                            OnlineUpdateProgress progress,
                            void* progressContext, char* error,
                            size_t errorSize) {
  if (scratch == nullptr || scratchSize < 128 || digest == nullptr) {
    setError(error, errorSize, "update arguments");
    return false;
  }
  OnlineUpdateManifest manifest{};
  if (!loadManifest(scratch, scratchSize, manifest, progress,
                    progressContext, error, errorSize)) {
    return false;
  }
  return installFirmware(scratch, scratchSize, manifest.sha256, digest,
                         progress, progressContext, error, errorSize);
}

}  // namespace zifi
