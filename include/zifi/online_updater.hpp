#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/net_client.hpp"

namespace zifi {

// Этапы, которые WMF-плагин показывает пользователю во время автономного OTA.
enum OnlineUpdateStage : uint8_t {
  kOnlineUpdateManifest = 1,
  kOnlineUpdateFirmware = 2,
  kOnlineUpdateVerify = 3,
};

using OnlineUpdateProgress = bool (*)(void* context, uint8_t stage,
                                      uint8_t percent);

constexpr size_t kOnlineUpdateVersionCapacity = 32;

struct OnlineUpdateManifest {
  uint8_t sha256[32];
  char version[kOnlineUpdateVersionCapacity];
};

// Загружает firmware.sha256 и firmware.bin из опубликованной ветки GitHub.
// Образ пишется только в неактивный OTA-раздел, а загрузочным становится после
// проверки ESP32-S3 application-заголовка и полного SHA-256.
class OnlineUpdater {
 public:
  explicit OnlineUpdater(NetClient& client);

  // Читает опубликованные версию и SHA без открытия flash на запись.
  bool check(uint8_t* scratch, size_t scratchSize,
             OnlineUpdateManifest& manifest, OnlineUpdateProgress progress,
             void* progressContext, char* error, size_t errorSize);

  bool install(uint8_t* scratch, size_t scratchSize, uint8_t digest[32],
               OnlineUpdateProgress progress, void* progressContext,
               char* error, size_t errorSize);

 private:
  bool loadManifest(uint8_t* scratch, size_t scratchSize,
                    OnlineUpdateManifest& manifest,
                    OnlineUpdateProgress progress, void* progressContext,
                    char* error, size_t errorSize);
  bool installFirmware(uint8_t* scratch, size_t scratchSize,
                       const uint8_t expectedSha[32], uint8_t digest[32],
                       OnlineUpdateProgress progress, void* progressContext,
                       char* error, size_t errorSize);

  NetClient& client_;
};

}  // namespace zifi
