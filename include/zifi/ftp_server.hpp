#pragma once

#include <WiFi.h>

#include <stddef.h>
#include <stdint.h>

#include "zifi/vfs_bridge.hpp"

namespace zifi {

// FTP-сервер с несколькими управляющими сессиями. Сокеты принадлежат сетевой
// задаче ядра 0, а единственный VFS-тракт последовательно обслуживает файловые
// команды через владельца UART на ядре 1.
class FtpServer {
 public:
  static constexpr size_t kMaxSessions = 3;
  static constexpr uint16_t kPassivePort = 2122;
  static constexpr uint16_t kLastPassivePort =
      kPassivePort + static_cast<uint16_t>(kMaxSessions) - 1;
  using EventSink = bool (*)(void* context, uint8_t command,
                             const uint8_t* data, uint16_t length);

  FtpServer(VfsBridge& bridge, EventSink eventSink, void* eventContext);

  // Тело команды совместимо с ZIFIFTP.WMF:
  // [port LE16][user\0][password\0], хвост необязателен.
  bool start(const uint8_t* payload, uint16_t length,
             uint16_t& actualPort, char* error, size_t errorSize);
  void stop();
  void poll();

  bool running() const { return running_; }
  uint16_t port() const { return port_; }
  size_t makeRamStats(uint8_t* output, size_t capacity) const;

 private:
  static constexpr size_t kMaxLine = 512;
  static constexpr size_t kMaxPath = 255;
  static constexpr size_t kDataChunk = 1024;

  using WaitHook = void (*)(void* context);

  struct Session {
    Session();

    WiFiServer passiveServer;
    WiFiClient controlClient;
    WiFiClient dataClient;

    bool active;
    bool passiveListening;
    bool loggedIn;
    bool userAccepted;
    bool discardLine;
    bool activeEndpointSet;
    bool pendingCommand;
    uint16_t passivePort;
    uint16_t activePort;
    IPAddress activeAddress;
    uint32_t lastControlActivityMs;

    char cwd[kMaxPath + 1];
    char line[kMaxLine + 1];
    size_t lineLength;
    char pendingLine[kMaxLine + 1];
  };

  struct StoreWaitContext {
    FtpServer* server;
    Session* session;
  };

  void acceptControl();
  void serviceSessions(Session* excluded = nullptr);
  void receiveControl(Session& session);
  void dispatchCommand(Session& session, char* line);
  void executeCommand(Session& session, char* line);
  void runPendingCommand();
  void dropControl(Session& session, const char* reason = nullptr);
  void sendClientState();
  void sendCommandEvent(const char* command, const char* argument);
  static bool commandUsesVfs(const char* line);

  bool reply(Session& session, const char* text);
  bool replyFormat(Session& session, const char* format, ...);
  static bool sendAll(WiFiClient& client, const uint8_t* data, size_t length,
                      uint32_t idleTimeoutMs);

  bool normalizePath(const Session& session, const char* argument,
                     char output[kMaxPath + 1]) const;
  bool parsePort(const char* argument, IPAddress& address, uint16_t& port) const;
  bool parseEprt(const char* argument, IPAddress& address,
                 uint16_t& port) const;

  void closePassive(Session& session);
  void closeData(Session& session);
  bool openData(Session& session);
  void enterPassive(Session& session, bool extended);
  void setActive(Session& session, const char* argument, bool extended);

  bool requestVfs(VfsOperation operation, const char* path, uint32_t value,
                  VfsResult& result, uint32_t timeoutMs,
                  WaitHook hook = nullptr, void* hookContext = nullptr);
  bool statPath(const char* path, VfsResult& result);
  bool resetBuffers();

  void changeDirectory(Session& session, const char* argument);
  void sendSize(Session& session, const char* argument);
  void list(Session& session, bool namesOnly);
  void retrieve(Session& session, const char* argument);
  void store(Session& session, const char* argument);
  void deleteFile(Session& session, const char* argument);
  void makeDirectory(Session& session, const char* argument);

  bool prefetchStore(Session& session);
  static void prefetchStoreThunk(void* context);

  VfsBridge& bridge_;
  EventSink eventSink_;
  void* eventContext_;
  WiFiServer controlServer_;
  Session sessions_[kMaxSessions];
  Session* vfsOwner_;

  bool running_;
  uint16_t port_;

  char user_[33];
  char password_[65];
  char lastVfsError_[64];

  uint8_t ioBuffer_[kDataChunk];
  bool storEof_;
  bool storError_;
  uint32_t storReceived_;
  uint32_t storLastProgressMs_;

  uint8_t ramStatCount_;
  uint32_t ramStatAt_[2];
  uint32_t ramStatFree_[2];
};

}  // namespace zifi
