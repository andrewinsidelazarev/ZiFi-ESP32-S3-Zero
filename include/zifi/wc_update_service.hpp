#pragma once

#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "zifi/net_client.hpp"
#include "zifi/wc_updater.hpp"

namespace zifi {

// HTTPS-загрузчик обновлятора WC: тот же проверяемый TLS с набором корневых
// сертификатов, что у онлайн-обновления прошивки.
class NetWcFetcher : public WcFetcher {
 public:
  // Флаг остановки сеанса: приём прерывается, как только он поднят, и STOP
  // не ждёт минуту тайм-аута HTTPS.
  void setCancel(const volatile bool* cancel) { cancel_ = cancel; }
  bool get(const char* host, const char* path, uint8_t* buffer,
           size_t capacity, size_t& length, uint16_t& status, char* error,
           size_t errorSize) override;

 private:
  bool cancelled() const { return cancel_ != nullptr && *cancel_; }

  NetClient client_;
  const volatile bool* cancel_ = nullptr;
};

// Сеанс обновления WC в собственной задаче ядра 0. HTTPS и VFS-запросы в нём
// блокирующие и долгие, поэтому сетевая задача их не исполняет: она только
// передаёт сюда команды плагина через очередь.
class WcUpdateService {
 public:
  WcUpdateService(VfsBridge& bridge, WcUpdater::EventSink sink, void* context);
  ~WcUpdateService();

  WcUpdateService(const WcUpdateService&) = delete;
  WcUpdateService& operator=(const WcUpdateService&) = delete;

  bool start(const uint8_t* payload, uint16_t length, char* error,
             size_t errorSize);
  // Номера файлов по байту. Принимается, пока сеанс ждёт команды и прежний
  // APPLY закончен: повтор той же команды плагином второй раз не запускает.
  bool apply(const uint8_t* payload, uint16_t length);
  // Выдать заново список и последнее состояние.
  bool sync();
  // Попросить сеанс закончиться и дождаться этого не дольше timeoutMs.
  bool stop(uint32_t timeoutMs);
  bool running() const { return task_ != nullptr; }

 private:
  struct Command {
    uint8_t kind;
    uint8_t count;
    uint8_t indices[WcUpdater::kMaxFiles];
  };
  static constexpr uint8_t kCommandApply = 1;
  static constexpr uint8_t kCommandStop = 2;
  static constexpr uint8_t kCommandSync = 3;

  static void taskEntry(void* context);
  void run();
  void release();

  VfsBridge& bridge_;
  WcUpdater::EventSink sink_;
  void* context_;
  NetWcFetcher* fetcher_;
  WcUpdater* updater_;
  QueueHandle_t commands_;
  TaskHandle_t task_;
  volatile bool finished_;
  volatile bool applying_;
};

}  // namespace zifi
