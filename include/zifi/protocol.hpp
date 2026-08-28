#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

constexpr uint8_t kSync = 0x5A;
constexpr size_t kMaxPayload = 1024;
constexpr uint32_t kFrameTimeoutMs = 500;

enum Command : uint8_t {
  kEcho = 0x00,
  kWifiConnect = 0x01,
  kSysInfo = 0x02,
  kWifiIni = 0x03,
  kPing = 0x04,
  kGetStep = 0x05,
  kFtpStart = 0x06,
  kFtpStop = 0x07,
  kUpdateStart = 0x08,
  kUpdateStop = 0x09,
  kFtpRamStats = 0x0A,
  kSmbStart = 0x0B,
  kSmbStop = 0x0C,
  kOnlineUpdate = 0x0D,
  kOnlineUpdateCheck = 0x0E,
  kSysReset = 0x0F,
  kNetOpen = 0x10,
  kNetSend = 0x11,
  kNetRecv = 0x12,
  kNetClose = 0x13,
  kNetHttpGet = 0x14,
  kNetPing = 0x20,
  kNetIpConfig = 0x21,
  kNetNtp = 0x22,
  kNetProxyStatus = 0x23,

  kVfsStat = 0x40,
  kVfsOpenDir = 0x41,
  kVfsReadDir = 0x42,
  kVfsFsInfo = 0x43,
  kVfsFatWindow = 0x44,
  kVfsOpen = 0x50,
  kVfsRead = 0x51,
  kVfsWriteLegacy = 0x52,
  kVfsClose = 0x53,
  kVfsDelete = 0x54,
  kVfsMkdir = 0x55,
  kVfsBlock = 0x56,
  kVfsWriteWindow = 0x57,
  kVfsReadWindow = 0x58,
  kVfsRename = 0x59,
  kVfsExtend = 0x5A,
  kVfsSeek = 0x5B,
  kVfsSetEof = 0x5C,
  kVfsMoveRename = 0x5D,
  kVfsSetMetadata = 0x5E,

  kEventFtpClient = 0x60,
  kEventFtpCommand = 0x61,
  kEventSmbClient = 0x62,
  kEventSmbCommand = 0x63,
  // Ход текущей передачи файла отдельной строкой. Идёт редко и с готовым
  // текстом, поэтому плагину не нужно ни считать байты, ни делить их.
  kEventSmbProgress = 0x64,
  // [этап][проценты] автономной загрузки firmware.bin с GitHub.
  kEventOnlineUpdateProgress = 0x65,
  // Готовая ASCII-шкала уровня Wi-Fi для поля Status SMB-плагина. Отдельное
  // событие не вызывает SYS_INFO и не смешивается с его диагностическим текстом.
  kEventWifiSignal = 0x66,

  kRespSysInfo = 0x82,
  kRespWifiConnect = 0x81,
  kRespWifiIni = 0x83,
  kRespGetStep = 0x85,
  kRespFtpStart = 0x86,
  kRespFtpStop = 0x87,
  kRespUpdateStart = 0x88,
  kRespUpdateStop = 0x89,
  kRespFtpRamStats = 0x8A,
  kRespSmbStart = 0x8B,
  kRespSmbStop = 0x8C,
  kRespOnlineUpdate = 0x8D,
  kRespOnlineUpdateCheck = 0x8E,
  kRespNetOpen = 0x90,
  kRespNetSend = 0x91,
  kRespNetRecv = 0x92,
  kRespNetClose = 0x93,
  kRespNetHttpGet = 0x94,
  kRespNetIpConfig = 0xA0,
  kRespNetPing = 0xA1,
  kRespNetNtp = 0xA2,
  kRespNetProxyStatus = 0xA3,
  kReady = 0xF0,
  kError = 0xEE,
  kAck = 0xFE,
};

enum OnlineUpdateRelation : uint8_t {
  kOnlineUpdateSame = 0,
  kOnlineUpdateNewer = 1,
  kOnlineUpdateOlder = 2,
  kOnlineUpdateDifferent = 3,
};

struct PacketView {
  uint8_t command = 0;
  const uint8_t* data = nullptr;
  uint16_t length = 0;
};

class FrameParser {
 public:
  FrameParser();

  bool feed(uint8_t byte, uint32_t nowMs, PacketView& packet);
  bool checkTimeout(uint32_t nowMs);
  void reset();

  uint32_t badChecksumCount() const { return badChecksumCount_; }
  uint32_t resyncCount() const { return resyncCount_; }

 private:
  enum State : uint8_t { kWaitSync, kCommand, kLengthLow, kLengthHigh, kData, kChecksum };

  State state_;
  uint8_t command_;
  uint16_t length_;
  uint16_t position_;
  uint8_t checksum_;
  uint32_t lastByteMs_;
  uint32_t badChecksumCount_;
  uint32_t resyncCount_;
  uint8_t payload_[kMaxPayload];
};

using ByteWriter = bool (*)(void* context, const uint8_t* data, size_t length);

bool writeFrame(ByteWriter writer, void* context, uint8_t command,
                const uint8_t* data = nullptr, uint16_t length = 0);

uint16_t crc16CcittFalse(const uint8_t* data, size_t length);
uint16_t crc16CcittFalseUpdate(uint16_t crc, const uint8_t* data,
                               size_t length);
uint32_t crc32IsoHdlc(const uint8_t* data, size_t length);

inline uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

inline uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

inline void writeLe16(uint8_t* data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
}

inline void writeLe32(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
  data[2] = static_cast<uint8_t>(value >> 16);
  data[3] = static_cast<uint8_t>(value >> 24);
}

}  // namespace zifi
