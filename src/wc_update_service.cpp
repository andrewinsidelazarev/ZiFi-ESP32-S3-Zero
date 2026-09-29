#include "zifi/wc_update_service.hpp"

#include <Arduino.h>

#include <new>
#include <stdio.h>
#include <string.h>

namespace zifi {
namespace {

// Рукопожатие mbedTLS идёт прямо на стеке задачи, как у сетевой задачи.
constexpr uint32_t kTaskStackBytes = 16384;
constexpr UBaseType_t kTaskPriority = 2;
constexpr BaseType_t kTaskCore = 0;
constexpr uint32_t kHttpTimeoutMs = 60000;

}  // namespace

bool NetWcFetcher::get(const char* host, const char* path, uint8_t* buffer,
                       size_t capacity, size_t& length, uint16_t& status,
                       char* error, size_t errorSize) {
  length = 0;
  status = 0;
  if (cancelled()) {
    snprintf(error, errorSize, "stopped");
    return false;
  }
  uint32_t contentLength = 0;
  if (!client_.httpGet(host, 443, path, status, contentLength, error,
                       errorSize)) {
    client_.close();
    return false;
  }
  const uint32_t started = millis();
  bool eof = false;
  while (!eof) {
    if (cancelled()) {
      client_.close();
      snprintf(error, errorSize, "stopped");
      return false;
    }
    if (length >= capacity) {
      client_.close();
      snprintf(error, errorSize, "body too long");
      return false;
    }
    size_t received = 0;
    if (!client_.receive(buffer + length, capacity - length, received, eof,
                         error, errorSize)) {
      client_.close();
      return false;
    }
    length += received;
    if (received == 0 && !eof) {
      if (static_cast<uint32_t>(millis() - started) >= kHttpTimeoutMs) {
        client_.close();
        snprintf(error, errorSize, "http timeout");
        return false;
      }
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  client_.close();
  return true;
}

WcUpdateService::WcUpdateService(VfsBridge& bridge, WcUpdater::EventSink sink,
                                 void* context)
    : bridge_(bridge),
      sink_(sink),
      context_(context),
      fetcher_(nullptr),
      updater_(nullptr),
      commands_(nullptr),
      task_(nullptr),
      finished_(true),
      applying_(false) {}

WcUpdateService::~WcUpdateService() { stop(1000); }

void WcUpdateService::release() {
  delete updater_;
  updater_ = nullptr;
  delete fetcher_;
  fetcher_ = nullptr;
  if (commands_ != nullptr) {
    vQueueDelete(commands_);
    commands_ = nullptr;
  }
  task_ = nullptr;
}

bool WcUpdateService::start(const uint8_t* payload, uint16_t length,
                            char* error, size_t errorSize) {
  if (task_ != nullptr) {
    snprintf(error, errorSize, "already running");
    return false;
  }
  fetcher_ = new (std::nothrow) NetWcFetcher();
  updater_ = fetcher_ == nullptr
                 ? nullptr
                 : new (std::nothrow) WcUpdater(bridge_, *fetcher_, sink_,
                                                context_);
  commands_ = xQueueCreate(2, sizeof(Command));
  if (fetcher_ == nullptr || updater_ == nullptr || commands_ == nullptr) {
    release();
    snprintf(error, errorSize, "no memory");
    return false;
  }
  if (!updater_->configure(payload, length, error, errorSize)) {
    release();
    return false;
  }
  fetcher_->setCancel(updater_->stopFlag());
  finished_ = false;
  applying_ = false;
  if (xTaskCreatePinnedToCore(taskEntry, "wc-update", kTaskStackBytes, this,
                              kTaskPriority, &task_, kTaskCore) != pdPASS) {
    finished_ = true;
    release();
    snprintf(error, errorSize, "no task");
    return false;
  }
  return true;
}

void WcUpdateService::taskEntry(void* context) {
  static_cast<WcUpdateService*>(context)->run();
}

void WcUpdateService::run() {
  // Итог проверки плагину уже сообщён событиями. Даже после ошибки сеанс
  // ждёт STOP: плагин держит на экране причину, пока пользователь не выйдет.
  updater_->check();
  Command command{};
  // Остановка проверяется и до, и после каждой команды: если STOP не влез в
  // очередь, ожидающий APPLY её не отменит.
  while (!updater_->stopRequested() &&
         xQueueReceive(commands_, &command, portMAX_DELAY) == pdTRUE) {
    if (command.kind == kCommandStop || updater_->stopRequested()) {
      break;
    }
    if (command.kind == kCommandApply) {
      updater_->apply(command.indices, command.count);
      applying_ = false;
    } else if (command.kind == kCommandSync) {
      updater_->resend();
    }
  }
  finished_ = true;
  vTaskDelete(nullptr);
}

bool WcUpdateService::apply(const uint8_t* payload, uint16_t length) {
  if (task_ == nullptr || finished_ || applying_ ||
      updater_->stopRequested() || length > WcUpdater::kMaxFiles) {
    return false;
  }
  Command command{};
  command.kind = kCommandApply;
  command.count = static_cast<uint8_t>(length);
  if (length != 0) {
    memcpy(command.indices, payload, length);
  }
  applying_ = true;
  if (xQueueSend(commands_, &command, 0) != pdTRUE) {
    applying_ = false;
    return false;
  }
  return true;
}

bool WcUpdateService::sync() {
  if (task_ == nullptr || finished_ || updater_->stopRequested()) {
    return false;
  }
  Command command{};
  command.kind = kCommandSync;
  return xQueueSend(commands_, &command, 0) == pdTRUE;
}

bool WcUpdateService::stop(uint32_t timeoutMs) {
  if (task_ == nullptr) {
    return true;
  }
  updater_->requestStop();
  Command command{};
  command.kind = kCommandStop;
  // Очередь из двух мест: STOP встанет даже за ещё не взятым APPLY.
  xQueueSend(commands_, &command, pdMS_TO_TICKS(100));
  const uint32_t started = millis();
  while (!finished_) {
    if (static_cast<uint32_t>(millis() - started) >= timeoutMs) {
      return false;  // задача ещё в обмене; память освободит следующий stop
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  // Задача удаляет себя сама сразу после finished_; даём ей это сделать.
  vTaskDelay(pdMS_TO_TICKS(20));
  release();
  return true;
}

}  // namespace zifi
