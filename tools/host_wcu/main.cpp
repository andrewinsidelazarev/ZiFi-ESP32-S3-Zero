// Стенд обновлятора WC на ПК.
//
// НАСТОЯЩИЕ WcUpdater, VfsBridge, VfsClient и двоичный протокол — те же файлы,
// что в прошивке. Подменены только края: вместо Z80 за «UART» стоит эмулятор
// Wild Commander поверх папки SD (tools/host_smb/z80_sim), вместо GitHub —
// папка, в которой лежат ответы API (_ref.json, _tree.json) и сами файлы.
//
// host_wcu.exe <sd> <github> <repo> <ветка> <каталог> <режим> [защищённые...]
//   режим check — только проверка; apply — проверить и обновить всё, что
//   можно обновить; auto — обновить только отмеченное по клавише A.
// События печатаются строками:
//   STATE <этап> <текущий> <всего> <процент> <текст>
//   ENTRY <номер> <состояние> <флаги> <на SD> <на GitHub> <путь>
//   HTTP <узел> <путь>
// ZIFI_WCU_CORRUPT_DOWNLOAD=<подстрока пути>: первое скачивание такого файла
// приходит испорченным — проверка повторов загрузки; ZIFI_WCU_CORRUPT_TIMES=N —
// первые N скачиваний.
// Режим retry: после APPLY всего обновляемого — второй APPLY только строк
// FAILED, чей путь содержит ZIFI_WCU_RETRY (повтор одного файла в сеансе).

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <timeapi.h>

#include "zifi/uart_transport.hpp"
#include "zifi/vfs_bridge.hpp"
#include "zifi/wc_updater.hpp"

#include "z80_sim.hpp"

#pragma comment(lib, "winmm.lib")

HardwareSerial Serial;
HardwareSerial Serial0;
EspClass ESP;
WiFiClassHost WiFi;
LittleFSClass LittleFS;

namespace {

std::mutex printLock;

void printLine(const std::string& line) {
  std::lock_guard<std::mutex> guard(printLock);
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

uint32_t readLe(const uint8_t* data, int bytes) {
  uint32_t value = 0;
  for (int index = bytes - 1; index >= 0; --index) {
    value = value << 8 | data[index];
  }
  return value;
}

bool printEvent(void*, uint8_t command, const uint8_t* data, uint16_t length) {
  char head[96];
  if (command == 0x67 && length >= 6) {
    std::snprintf(head, sizeof(head), "STATE %u %u %u %u ", data[0],
                  static_cast<unsigned>(readLe(data + 1, 2)),
                  static_cast<unsigned>(readLe(data + 3, 2)), data[5]);
    printLine(head + std::string(reinterpret_cast<const char*>(data + 6),
                                 length - 6));
    return true;
  }
  if (command == 0x68 && length >= 9) {
    std::snprintf(head, sizeof(head), "ENTRY %u %u %u %lu %lu ", data[0],
                  data[1], data[2],
                  static_cast<unsigned long>(readLe(data + 3, 3)),
                  static_cast<unsigned long>(readLe(data + 6, 3)));
    printLine(head + std::string(reinterpret_cast<const char*>(data + 9),
                                 length - 9));
    return true;
  }
  std::snprintf(head, sizeof(head), "EVENT %02X %u", command, length);
  printLine(head);
  return true;
}

std::string decodePercent(const std::string& text) {
  std::string output;
  for (size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '%' && index + 2 < text.size()) {
      output.push_back(static_cast<char>(
          std::strtoul(text.substr(index + 1, 2).c_str(), nullptr, 16)));
      index += 2;
    } else {
      output.push_back(text[index]);
    }
  }
  return output;
}

// «GitHub» из папки: ответы API лежат файлами, raw-запрос отдаёт файл дерева.
class FolderFetcher : public zifi::WcFetcher {
 public:
  explicit FolderFetcher(std::string root) : root_(std::move(root)) {
    const char* corrupt = std::getenv("ZIFI_WCU_CORRUPT_DOWNLOAD");
    corrupt_ = corrupt == nullptr ? "" : corrupt;
    const char* times = std::getenv("ZIFI_WCU_CORRUPT_TIMES");
    corruptTimes_ = times == nullptr ? 1 : std::atoi(times);
  }

  bool get(const char* host, const char* path, uint8_t* buffer,
           size_t capacity, size_t& length, uint16_t& status, char* error,
           size_t errorSize) override {
    printLine(std::string("HTTP ") + host + " " + path);
    length = 0;
    std::string file;
    if (std::strcmp(host, "api.github.com") == 0) {
      if (std::strstr(path, "/git/ref/heads/") != nullptr) {
        file = root_ + "/_ref.json";
      } else if (std::strstr(path, "/git/trees/") != nullptr) {
        file = root_ + "/_tree.json";
      }
    } else if (std::strcmp(host, "raw.githubusercontent.com") == 0) {
      // /<владелец>/<репозиторий>/<коммит>/<каталог и путь>
      const std::string text(path);
      size_t slash = 0;
      for (int part = 0; part < 4 && slash != std::string::npos; ++part) {
        slash = text.find('/', slash + (part == 0 ? 0 : 1));
      }
      if (slash != std::string::npos) {
        file = root_ + "/" + decodePercent(text.substr(slash + 1));
      }
    }
    std::FILE* input = nullptr;
    if (file.empty() || fopen_s(&input, file.c_str(), "rb") != 0 ||
        input == nullptr) {
      status = 404;
      return true;
    }
    length = std::fread(buffer, 1, capacity, input);
    const bool longer = std::fgetc(input) != EOF;
    std::fclose(input);
    if (longer) {
      std::snprintf(error, errorSize, "body too long");
      return false;
    }
    status = 200;
    if (!corrupt_.empty() && std::strstr(path, corrupt_.c_str()) != nullptr &&
        length != 0 && corruptTimes_ > 0) {
      buffer[length / 2] ^= 0xA5;  // испорчены только первые скачивания
      --corruptTimes_;
    }
    return true;
  }

 private:
  std::string root_;
  std::string corrupt_;
  int corruptTimes_ = 1;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 7) {
    std::printf("usage: host_wcu <sd> <github> <repo> <branch> <dir> "
                "<check|apply|auto> [protected...]\n");
    return 2;
  }
  timeBeginPeriod(1);
  const std::string mode = argv[6];

  zifi::host::Z80Simulator simulator(Serial, argv[1]);
  // Как плагин WC Update: OPEN на запись занятое имя не берёт.
  simulator.setWriteNewOnly(true);
  zifi::UartTransport transport(Serial);
  transport.begin();
  zifi::VfsBridge bridge(transport);
  if (!bridge.begin(true)) {
    printLine("RESULT 2 bridge");
    return 2;
  }
  FolderFetcher fetcher(argv[2]);
  zifi::WcUpdater updater(bridge, fetcher, printEvent, nullptr);

  std::vector<uint8_t> payload;
  for (int index = 3; index < argc; ++index) {
    if (index == 6) {
      continue;  // режим стенда в запрос обновлятора не входит
    }
    payload.insert(payload.end(), argv[index],
                   argv[index] + std::strlen(argv[index]) + 1);
  }
  payload.push_back(0);
  char error[64] = {};
  if (!updater.configure(payload.data(), static_cast<uint16_t>(payload.size()),
                         error, sizeof(error))) {
    printLine(std::string("RESULT 2 ") + error);
    return 2;
  }

  std::atomic<bool> done{false};
  bool ok = false;
  std::thread worker([&] {
    ok = updater.check();
    if (ok && mode == "sync") {
      ok = updater.resend();
    } else if (ok && mode != "check") {
      std::vector<uint8_t> indices;
      for (size_t index = 0; index < updater.fileCount(); ++index) {
        const zifi::WcFile* file = updater.file(index);
        if (mode == "apply" ? zifi::WcUpdater::canUpdate(*file)
                            : zifi::WcUpdater::autoSelect(*file)) {
          indices.push_back(static_cast<uint8_t>(index));
        }
      }
      ok = updater.apply(indices.data(), indices.size());
      const char* retry = std::getenv("ZIFI_WCU_RETRY");
      if (mode == "retry" && retry != nullptr) {
        indices.clear();
        for (size_t index = 0; index < updater.fileCount(); ++index) {
          const zifi::WcFile* file = updater.file(index);
          if (file->status == zifi::WcStatus::kFailed &&
              std::strstr(file->path, retry) != nullptr) {
            indices.push_back(static_cast<uint8_t>(index));
          }
        }
        printLine("RETRY");
        ok = updater.apply(indices.data(), indices.size());
      }
    }
    done = true;
  });
  // Мост исполняется на «ядре 1»: на ПК это главный поток.
  while (!done) {
    bridge.pollCore1();
    bridge.waitForRequest(1);
  }
  worker.join();
  printLine(std::string("RESULT ") + (ok ? "0 " : "1 ") + updater.lastError());
  return ok ? 0 : 1;
}
