#include "zifi/ota_server.hpp"

#include <Arduino.h>
#include <Update.h>
#include <mbedtls/sha256.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zifi/ota_image.hpp"
#include "zifi/diagnostic_log.hpp"

namespace zifi {
namespace {

constexpr uint32_t kBeginTimeoutMs = 10000;
constexpr uint32_t kDataIdleTimeoutMs = 30000;
constexpr uint32_t kDataSocketTimeoutSeconds = 30;
constexpr uint32_t kSendIdleTimeoutMs = 5000;

void setExternalError(char* error, size_t errorSize, const char* text) {
  if (error != nullptr && errorSize != 0) {
    snprintf(error, errorSize, "%s", text);
  }
}

int hexValue(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

}  // namespace

OtaServer::OtaServer()
    : server_(kDefaultPort, 1),
      client_(),
      running_(false),
      clientActive_(false),
      port_(kDefaultPort),
      acceptedAtMs_(0),
      line_{},
      lineLength_(0),
      chunk_{} {}

bool OtaServer::start(uint16_t port, char* error, size_t errorSize) {
  stop();
  if (port == 0 || WiFi.status() != WL_CONNECTED) {
    setExternalError(error, errorSize,
                     port == 0 ? "ota port zero" : "ota no wifi");
    return false;
  }
  port_ = port;
  server_.begin(port_, 1);
  server_.setNoDelay(true);
  if (!server_) {
    server_.stop();
    setExternalError(error, errorSize, "ota listen failed");
    return false;
  }
  running_ = true;
  diagnosticLogEvent("OTA started port=%u", static_cast<unsigned>(port_));
  setExternalError(error, errorSize, "");
  return true;
}

void OtaServer::closeClient() {
  // operator bool у WiFiClient после FIN уже может вернуть false, хотя объект
  // ещё владеет сокетом. stop вызывается без условия, чтобы fd не протёк.
  client_.stop();
  client_ = WiFiClient();
  clientActive_ = false;
  lineLength_ = 0;
}

void OtaServer::stop() {
  closeClient();
  server_.stop();
  running_ = false;
}

bool OtaServer::sendBuffer(const uint8_t* data, size_t length) {
  if (!clientActive_ || (data == nullptr && length != 0)) {
    return false;
  }
  size_t offset = 0;
  uint32_t idleSince = millis();
  while (offset < length) {
    const size_t written = client_.write(data + offset, length - offset);
    if (written != 0) {
      offset += written;
      idleSince = millis();
      yield();
      continue;
    }
    if (!client_.connected() ||
        static_cast<uint32_t>(millis() - idleSince) >= kSendIdleTimeoutMs) {
      return false;
    }
    delay(2);
  }
  return true;
}

bool OtaServer::sendText(const char* text) {
  return text != nullptr && sendBuffer(
      reinterpret_cast<const uint8_t*>(text), strlen(text));
}

bool OtaServer::sendFormat(const char* format, ...) {
  char text[160];
  va_list arguments;
  va_start(arguments, format);
  const int length = vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  return length > 0 && static_cast<size_t>(length) < sizeof(text) &&
         sendText(text);
}

void OtaServer::acceptClient() {
  if (!server_.hasClient()) {
    return;
  }
  WiFiClient guest = server_.accept();
  if (!guest) {
    return;
  }
  if (clientActive_ && (client_.connected() || client_.available() > 0)) {
    guest.write(reinterpret_cast<const uint8_t*>("ERR busy\n"), 9);
    guest.stop();
    return;
  }
  closeClient();
  client_ = guest;
  clientActive_ = true;
  diagnosticLogEvent("OTA client-accepted");
  client_.setNoDelay(true);
  // В Arduino-ESP32 setTimeout принимает секунды, а не миллисекунды.
  client_.setTimeout(kDataSocketTimeoutSeconds);
  acceptedAtMs_ = millis();
  lineLength_ = 0;
  sendFormat("ZIFI-OTA/1 %s %lu\n", ZIFI_BUILD_VERSION,
             static_cast<unsigned long>(ESP.getFreeSketchSpace()));
}

bool OtaServer::parseSha256(const char* text, uint8_t output[32]) {
  if (text == nullptr || strlen(text) != 64) {
    return false;
  }
  for (size_t i = 0; i < 32; ++i) {
    const int high = hexValue(text[i * 2]);
    const int low = hexValue(text[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    output[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool OtaServer::receiveFirmware(uint32_t size,
                                const uint8_t expectedSha[32]) {
  diagnosticLogEvent("OTA firmware-begin bytes=%lu",
                     static_cast<unsigned long>(size));
  if (size < kOtaImageHeaderSize || size > ESP.getFreeSketchSpace()) {
    sendText("ERR image does not fit\n");
    return false;
  }

  mbedtls_sha256_context shaContext;
  mbedtls_sha256_init(&shaContext);
  if (mbedtls_sha256_starts_ret(&shaContext, 0) != 0) {
    mbedtls_sha256_free(&shaContext);
    sendText("ERR sha256 init\n");
    return false;
  }
  if (!sendFormat("READY %u\n", static_cast<unsigned>(kChunkSize))) {
    mbedtls_sha256_free(&shaContext);
    return false;
  }

  uint32_t received = 0;
  size_t headerLength = 0;
  bool updateBegun = false;
  uint32_t idleSince = millis();
  while (received < size) {
    const int available = client_.available();
    if (available > 0) {
      const uint32_t remaining = size - received;
      const bool collectingHeader = headerLength < kOtaImageHeaderSize;
      uint8_t* destination = collectingHeader ? chunk_ + headerLength : chunk_;
      size_t capacity = collectingHeader
                            ? kOtaImageHeaderSize - headerLength
                            : sizeof(chunk_);
      size_t wanted = static_cast<size_t>(available) < capacity
                          ? static_cast<size_t>(available)
                          : capacity;
      if (wanted > remaining) {
        wanted = static_cast<size_t>(remaining);
      }
      const int count = client_.read(destination, wanted);
      if (count <= 0) {
        sendText("ERR socket read\n");
        if (updateBegun) {
          Update.abort();
        }
        mbedtls_sha256_free(&shaContext);
        return false;
      }
      received += static_cast<uint32_t>(count);
      idleSince = millis();

      if (collectingHeader) {
        headerLength += static_cast<size_t>(count);
        if (headerLength < kOtaImageHeaderSize) {
          continue;
        }
        // До проверки application-заголовка flash вообще не стирается.
        if (!isEsp32S3ApplicationImage(chunk_, headerLength)) {
          sendText("ERR not ESP32-S3 application image\n");
          mbedtls_sha256_free(&shaContext);
          return false;
        }
        // begin выбирает неактивный OTA-раздел. До успешного end загрузочная
        // запись не меняется даже при reset или исчезновении питания.
        if (!Update.begin(size, U_FLASH)) {
          sendFormat("ERR update begin %u\n", Update.getError());
          Update.abort();
          mbedtls_sha256_free(&shaContext);
          return false;
        }
        updateBegun = true;
        if (Update.write(chunk_, headerLength) != headerLength ||
            mbedtls_sha256_update_ret(&shaContext, chunk_, headerLength) != 0) {
          sendFormat("ERR first block %u\n", Update.getError());
          Update.abort();
          mbedtls_sha256_free(&shaContext);
          return false;
        }
      } else {
        // Сначала пишем staging, затем включаем байты в SHA. При короткой
        // записи digest не должен выглядеть как digest полного файла.
        if (Update.write(chunk_, static_cast<size_t>(count)) !=
                static_cast<size_t>(count) ||
            mbedtls_sha256_update_ret(&shaContext, chunk_,
                                      static_cast<size_t>(count)) != 0) {
          sendFormat("ERR flash write %u\n", Update.getError());
          Update.abort();
          mbedtls_sha256_free(&shaContext);
          return false;
        }
      }
      yield();
      continue;
    }
    if (!client_.connected()) {
      sendFormat("ERR short image %lu/%lu\n",
                 static_cast<unsigned long>(received),
                 static_cast<unsigned long>(size));
      if (updateBegun) {
        Update.abort();
      }
      mbedtls_sha256_free(&shaContext);
      return false;
    }
    if (static_cast<uint32_t>(millis() - idleSince) >= kDataIdleTimeoutMs) {
      sendFormat("ERR data timeout %lu/%lu\n",
                 static_cast<unsigned long>(received),
                 static_cast<unsigned long>(size));
      if (updateBegun) {
        Update.abort();
      }
      mbedtls_sha256_free(&shaContext);
      return false;
    }
    delay(2);
    yield();
  }

  uint8_t actualSha[32];
  if (mbedtls_sha256_finish_ret(&shaContext, actualSha) != 0) {
    sendText("ERR sha256 finish\n");
    Update.abort();
    mbedtls_sha256_free(&shaContext);
    return false;
  }
  mbedtls_sha256_free(&shaContext);
  uint8_t difference = 0;
  for (size_t i = 0; i < sizeof(actualSha); ++i) {
    difference |= actualSha[i] ^ expectedSha[i];
  }
  if (difference != 0) {
    // Полный, но неверный образ остаётся неактивным и явно отбрасывается.
    Update.abort();
    sendText("ERR sha256 mismatch\n");
    return false;
  }

  if (!Update.end(false)) {
    sendFormat("ERR update end %u\n", Update.getError());
    Update.abort();
    return false;
  }
  char digest[65];
  static constexpr char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(actualSha); ++i) {
    digest[i * 2] = kHex[actualSha[i] >> 4];
    digest[i * 2 + 1] = kHex[actualSha[i] & 0x0F];
  }
  digest[64] = 0;
  sendFormat("OK %s\n", digest);
  diagnosticLogEvent("OTA firmware-verified bytes=%lu",
                     static_cast<unsigned long>(size));
  delay(250);
  ESP.restart();
  return true;
}

void OtaServer::processBegin() {
  if (strcmp(line_, "LOG") == 0) {
    sendDiagnosticLog();
    closeClient();
    return;
  }
  if (strncmp(line_, "BEGIN ", 6) != 0) {
    sendText("ERR expected BEGIN or LOG\n");
    closeClient();
    return;
  }
  char* sizeText = line_ + 6;
  char* space = strchr(sizeText, ' ');
  if (space == nullptr) {
    sendText("ERR bad BEGIN\n");
    closeClient();
    return;
  }
  *space++ = 0;
  char* tail = nullptr;
  const unsigned long parsedSize = strtoul(sizeText, &tail, 10);
  uint8_t expectedSha[32];
  if (tail == sizeText || *tail != 0 || parsedSize == 0 ||
      parsedSize > UINT32_MAX || !parseSha256(space, expectedSha)) {
    sendText("ERR bad BEGIN\n");
    closeClient();
    return;
  }
  receiveFirmware(static_cast<uint32_t>(parsedSize), expectedSha);
  closeClient();
}

bool OtaServer::sendDiagnosticLog() {
  // В режим updater SMB уже остановлен, поэтому снимок не конкурирует с
  // записью событий. Сначала включаем в журнал сам запрос чтения, затем
  // отдаём неизменяемую копию из PSRAM.
  diagnosticLogEvent("LOG download-request");
  DiagnosticLogSnapshot snapshot{};
  if (!diagnosticLogSnapshot(snapshot)) {
    sendText("ERR log snapshot\n");
    return false;
  }

  uint8_t digest[32];
  mbedtls_sha256_context shaContext;
  mbedtls_sha256_init(&shaContext);
  const bool hashed =
      mbedtls_sha256_starts_ret(&shaContext, 0) == 0 &&
      mbedtls_sha256_update_ret(&shaContext, snapshot.data,
                                snapshot.length) == 0 &&
      mbedtls_sha256_finish_ret(&shaContext, digest) == 0;
  mbedtls_sha256_free(&shaContext);
  if (!hashed) {
    diagnosticLogFreeSnapshot(snapshot);
    sendText("ERR log sha256\n");
    return false;
  }

  static constexpr char kHex[] = "0123456789abcdef";
  char digestText[65];
  for (size_t index = 0; index < sizeof(digest); ++index) {
    digestText[index * 2] = kHex[digest[index] >> 4];
    digestText[index * 2 + 1] = kHex[digest[index] & 0x0f];
  }
  digestText[64] = 0;

  const bool sent = sendFormat("LOG %lu %s\n",
                               static_cast<unsigned long>(snapshot.length),
                               digestText) &&
                    sendBuffer(snapshot.data, snapshot.length) &&
                    sendText("OK\n");
  diagnosticLogFreeSnapshot(snapshot);
  return sent;
}

void OtaServer::receiveBeginLine() {
  if (!clientActive_) {
    return;
  }
  while (client_.available() > 0) {
    const int value = client_.read();
    if (value < 0) {
      break;
    }
    if (value == '\r') {
      continue;
    }
    if (value == '\n') {
      line_[lineLength_] = 0;
      processBegin();
      return;
    }
    if (lineLength_ >= kLineSize) {
      sendText("ERR line too long\n");
      closeClient();
      return;
    }
    line_[lineLength_++] = static_cast<char>(value);
  }
  if (static_cast<uint32_t>(millis() - acceptedAtMs_) >= kBeginTimeoutMs) {
    sendText("ERR BEGIN timeout\n");
    closeClient();
  } else if (client_.available() == 0 && !client_.connected()) {
    closeClient();
  }
}

void OtaServer::poll() {
  if (!running_) {
    return;
  }
  acceptClient();
  receiveBeginLine();
}

}  // namespace zifi
