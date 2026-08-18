#pragma once

// Эмулятор стороны Z80 (Wild Commander) для нативной сборки SMB-сервера.
// Подключается к HardwareSerial-заглушке и отвечает настоящими кадрами
// двоичного протокола ZiFi поверх обычной папки на диске.

#include <Arduino.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace zifi {
namespace host {

class Z80Simulator {
 public:
  Z80Simulator(HardwareSerial& serial, const std::string& root);

 private:
  struct Entry {
    std::string name;
    uint32_t size = 0;
    bool directory = false;
  };

  // Состояния разбора кадра повторяют приёмник плагина: пакет принимается
  // только целиком и только с верной контрольной суммой.
  enum class State {
    kSync,
    kCommand,
    kLengthLow,
    kLengthHigh,
    kPayload,
    kChecksum,
  };

  static void sinkThunk(void* context, const uint8_t* data, size_t length);
  void consume(const uint8_t* data, size_t length);
  void handle(uint8_t command, const std::vector<uint8_t>& payload);
  void reply(uint8_t command, const uint8_t* data, size_t length);
  void replyStatus(uint8_t command, uint8_t status);
  std::filesystem::path resolve(const std::string& path) const;

  void handleStat(const std::vector<uint8_t>& payload);
  void handleOpen(const std::vector<uint8_t>& payload);
  void handleSeek(const std::vector<uint8_t>& payload);
  void handleReadWindow(const std::vector<uint8_t>& payload);
  void handleWriteWindow(const std::vector<uint8_t>& payload);
  void handleSetEof(const std::vector<uint8_t>& payload);
  void handleClose();
  void handleOpenDir(const std::vector<uint8_t>& payload);
  void handleReadDir();
  void handleFsInfo();
  void handleMkdir(const std::vector<uint8_t>& payload);
  void handleDelete(const std::vector<uint8_t>& payload);

  HardwareSerial& serial_;
  std::string root_;

  State state_ = State::kSync;
  uint8_t command_ = 0;
  uint8_t checksum_ = 0;
  uint16_t expected_ = 0;
  std::vector<uint8_t> payload_;

  std::vector<Entry> entries_;
  size_t entryIndex_ = 0;

  // Открытый файл позиционного режима FILEX. На Z80 это FAT-контекст, здесь —
  // обычный путь плюс текущее смещение, заданное командой SEEK.
  std::filesystem::path openPath_;
  bool openValid_ = false;
  uint8_t openMode_ = 0;
  uint32_t offset_ = 0;

  // Приём окна записи: кадры собираются в буфер и сбрасываются на диск целиком
  // после проверки CRC — ровно так же, как это делает плагин.
  std::vector<uint8_t> windowData_;
  uint16_t windowTotal_ = 0;
  uint8_t windowSeq_ = 0;
  bool windowActive_ = false;
};

}  // namespace host
}  // namespace zifi
