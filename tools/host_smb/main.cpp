// Точка входа нативного симулятора SMB-сервера.
//
// Поднимает НАСТОЯЩИЙ SmbServer на этом ПК: libsmb2, обработчики, VfsBridge,
// VfsClient и двоичный протокол — те же самые файлы, что собираются в прошивку.
// Подменено только «дно»: вместо Z80 за UART стоит эмулятор Wild Commander
// поверх обычной папки.
//
// Запуск:
//   host_smb.exe <папка-ресурса> [порт] [байт/с] [имя]
//                [задержка каталога, мс] [задержка SET_EOF, мс]
//
// К нему подключается обычный Проводник Windows по \\127.0.0.1\SD, и любая
// поломка ловится отладчиком с полным стеком вместо reset_reason=4.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <Arduino.h>
#include <LittleFS.h>
#include <timeapi.h>
#include <WiFi.h>
#include <WiFiServer.h>
#include <cstring>

#include "zifi/smb_server.hpp"
#include "zifi/uart_transport.hpp"
#include "zifi/vfs_bridge.hpp"

#include "z80_sim.hpp"

// timeBeginPeriod живёт в winmm; сборочный скрипт её не перечисляет.
#pragma comment(lib, "winmm.lib")

namespace zifi {
namespace host {
// Перехватчик аварийного завершения: печатает стек с именами функций и
// строками. Ради него симулятор и собирается с отладочной информацией.
void installCrashReporter();
}  // namespace host
}  // namespace zifi

// Экземпляры, которых на хосте нет в системных библиотеках.
HardwareSerial Serial;
HardwareSerial Serial0;
EspClass ESP;
WiFiClassHost WiFi;
LittleFSClass LittleFS;

namespace {

// События сервера в обычную консоль. На железе они уходят в окно плагина по
// UART; здесь их печать безопасна и заменяет собой строки Last и Copying.
bool printEvent(void*, uint8_t event, const uint8_t* data, uint16_t length) {
  std::string text(reinterpret_cast<const char*>(data),
                   data == nullptr ? 0 : length);
  const char* kind = event == 0x62   ? "CLIENT"
                     : event == 0x63 ? "LAST"
                     : event == 0x64 ? "COPYING"
                                     : "EVENT";
  std::printf("[%s] %s\n", kind, text.c_str());
  std::fflush(stdout);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("Использование: host_smb.exe <папка-ресурса> [порт]\n");
    return 1;
  }
  const std::string root = argv[1];
  const uint16_t port =
      argc >= 3 ? static_cast<uint16_t>(std::atoi(argv[2])) : 445;
  const char* hostname = argc >= 5 ? argv[4] : "ZX-Evo";

  zifi::host::installCrashReporter();

  // Windows требует инициализировать WinSock до первого сокета. На ESP этого
  // шага нет, поэтому в самой прошивке его никто не делает.
  // Шаг системного таймера Windows по умолчанию 15,6 мс: без этого каждый
  // select() и sleep стенда длились бы на порядок дольше, чем на ESP32.
  timeBeginPeriod(1);

  WSADATA winsock = {};
  if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
    std::printf("WSAStartup не удался\n");
    return 1;
  }

  // Эмулятор Z80 садится на «UART» до старта сервера: первый же STAT должен
  // получить ответ, иначе сервер решит, что плагин молчит.
  zifi::host::Z80Simulator simulator(Serial, root);
  // Третий аргумент — скорость линии в байтах в секунду. Настоящий канал даёт
  // около 6000; без этого эмулятор отвечает мгновенно и гонки не возникают.
  if (argc >= 4) {
    simulator.setThrottle(static_cast<unsigned>(std::atoi(argv[3])));
  }
  if (argc >= 6) {
    simulator.setDirectoryDelay(static_cast<unsigned>(std::atoi(argv[5])));
  }
  if (argc >= 7) {
    simulator.setSetEofDelay(static_cast<unsigned>(std::atoi(argv[6])));
  }
  if (argc >= 9) {
    simulator.setWriteDelay(
        static_cast<unsigned>(std::atoi(argv[7])),
        static_cast<uint32_t>(std::strtoul(argv[8], nullptr, 10)));
  }

  zifi::UartTransport transport(Serial);
  transport.begin();

  zifi::VfsBridge bridge(transport);
  if (!bridge.begin(true)) {
    std::printf("Не удалось поднять VFS-мост\n");
    return 1;
  }

  zifi::SmbServer server(bridge, printEvent, nullptr);
  // Пояс ESP из zifi.ini (time:). В FAT эмулятор хранит поля как есть, а SMB
  // передаёт UTC, поэтому пояс сдвигает время в обе стороны.
  if (const char* timezone = std::getenv("ZIFI_HOST_TZ_HOURS")) {
    server.setTimezoneHours(static_cast<int8_t>(std::atoi(timezone)));
  }

  // Тело команды SMB_START повторяет то, что шлёт плагин: порт, ресурс, имя,
  // рабочая группа, логин и пароль. Так проверяется и разбор этой команды.
  uint8_t payload[64] = {};
  size_t offset = 0;
  payload[offset++] = static_cast<uint8_t>(port);
  payload[offset++] = static_cast<uint8_t>(port >> 8);
  for (const char* field : {"SD", hostname, "WORKGROUP", "zx", "zx"}) {
    const size_t size = std::strlen(field) + 1;
    std::memcpy(payload + offset, field, size);
    offset += size;
  }

  uint16_t actualPort = 0;
  bool netbios = false;
  char error[96] = {};
  if (!server.start(payload, static_cast<uint16_t>(offset), actualPort,
                    netbios, error, sizeof(error))) {
    std::printf("SMB не поднялся: %s\n", error);
    return 1;
  }

  std::printf("SMB-симулятор слушает порт %u, ресурс \\\\127.0.0.1\\SD -> %s\n",
              actualPort, root.c_str());
  std::printf("Вход: zx / zx. Остановка — Ctrl+C.\n");
  std::fflush(stdout);

  // Мост исполняется на «ядре 1»: на хосте это просто главный поток. Пауза
  // та же, что в прошивке: до 1 мс, но заявка SMB-потока будит сразу.
  while (true) {
    bridge.pollCore1();
    server.pollDiscovery();
    bridge.waitForRequest(1);
  }
  return 0;
}
