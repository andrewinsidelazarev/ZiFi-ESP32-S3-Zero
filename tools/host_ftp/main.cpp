// Нативный стенд FTP-сервера прошивки.
//
// Поднимает НАСТОЯЩИЙ FtpServer на этом ПК: разбор команд, VfsBridge, VfsClient
// и двоичный протокол — те же файлы, что собираются в прошивку. Подменено
// только «дно»: вместо Z80 за UART стоит эмулятор Wild Commander поверх
// обычной папки (tools\host_smb\z80_sim.cpp), а Wi-Fi — сокетами Windows.
//
// Запуск:
//   host_ftp.exe <папка-корень> [порт] [пояс, часы]
//
// К нему подключается любой FTP-клиент по 127.0.0.1, вход zx / zx.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>

#include "zifi/ftp_server.hpp"
#include "zifi/uart_transport.hpp"
#include "zifi/vfs_bridge.hpp"

#include "z80_sim.hpp"

namespace zifi {
namespace host {
void installCrashReporter();
}  // namespace host
}  // namespace zifi

HardwareSerial Serial;
HardwareSerial Serial0;
EspClass ESP;
WiFiClassHost WiFi;
LittleFSClass LittleFS;

namespace {

// События сервера (клиент, последняя команда) в обычную консоль. На железе
// они уходят в окно плагина по UART.
bool printEvent(void*, uint8_t event, const uint8_t* data, uint16_t length) {
  std::string text(reinterpret_cast<const char*>(data),
                   data == nullptr ? 0 : length);
  std::printf("[EVENT %02X] %s\n", event, text.c_str());
  std::fflush(stdout);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("Использование: host_ftp.exe <папка-корень> [порт] [пояс]\n");
    return 1;
  }
  const std::string root = argv[1];
  const uint16_t port =
      argc >= 3 ? static_cast<uint16_t>(std::atoi(argv[2])) : 2121;
  const int timezoneHours = argc >= 4 ? std::atoi(argv[3]) : 0;

  zifi::host::installCrashReporter();

  WSADATA winsock = {};
  if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
    std::printf("WSAStartup не удался\n");
    return 1;
  }

  zifi::host::Z80Simulator simulator(Serial, root);
  zifi::UartTransport transport(Serial);
  transport.begin();

  zifi::VfsBridge bridge(transport);
  if (!bridge.begin(true)) {
    std::printf("Не удалось поднять VFS-мост\n");
    return 1;
  }

  zifi::FtpServer server(bridge, printEvent, nullptr);
  server.setTimezoneHours(static_cast<int8_t>(timezoneHours));

  // Тело FTP_START, как его шлёт ZIFIFTP.WMF: [порт LE16][логин,0][пароль,0].
  uint8_t payload[32] = {};
  size_t offset = 0;
  payload[offset++] = static_cast<uint8_t>(port);
  payload[offset++] = static_cast<uint8_t>(port >> 8);
  for (const char* field : {"zx", "zx"}) {
    const size_t size = std::strlen(field) + 1;
    std::memcpy(payload + offset, field, size);
    offset += size;
  }
  uint16_t actualPort = 0;
  char error[96] = {};
  if (!server.start(payload, static_cast<uint16_t>(offset), actualPort, error,
                    sizeof(error))) {
    std::printf("FTP не поднялся: %s\n", error);
    return 1;
  }
  std::printf("FTP-стенд слушает 127.0.0.1:%u, корень %s, пояс %+d\n",
              actualPort, root.c_str(), timezoneHours);
  std::fflush(stdout);

  // Сетевая сторона — «ядро 0»: FtpServer::poll ждёт результатов VFS и сам
  // крутит сокеты. Мост исполняется на «ядре 1» — здесь это главный поток.
  std::atomic<bool> running{true};
  std::thread network([&]() {
    while (running.load()) {
      server.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  while (true) {
    bridge.pollCore1();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  running = false;
  network.join();
  return 0;
}
