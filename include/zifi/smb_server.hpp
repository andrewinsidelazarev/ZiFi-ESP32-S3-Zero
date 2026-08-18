#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi\vfs_bridge.hpp"

namespace zifi {

// Современный SMB2/SMB3-сервер работает в отдельной задаче сетевого ядра.
// Библиотека libsmb2 разбирает пакеты, NTLMSSP и подпись, а этот адаптер
// переводит файловые операции в последовательный UART/VFS Wild Commander.
class SmbServer {
 public:
  using EventSink = bool (*)(void* context, uint8_t command,
                             const uint8_t* data, uint16_t length);

  SmbServer(VfsBridge& bridge, EventSink eventSink, void* eventContext);
  ~SmbServer();

  // Тело SMB_START:
  // [port LE16][share\0][hostname\0][workgroup\0][user\0][password\0].
  // Все строки можно опустить — тогда используются безопасные значения по
  // умолчанию, совпадающие с настройками готового WMF-плагина.
  bool start(const uint8_t* payload, uint16_t length, uint16_t& actualPort,
             bool& netbiosActive, char* error, size_t errorSize);
  bool stop();

  // NBNS и WS-Discovery обслуживаются из уже существующей сетевой задачи.
  // Сам SMB listener живёт отдельно, потому что цикл libsmb2 блокирующий.
  void pollDiscovery();

  bool running() const;
  uint16_t port() const;
  bool netbiosActive() const;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace zifi
