#include "zifi/ftp_server.hpp"

#include <Arduino.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "zifi/fat_time.hpp"
#include "zifi/protocol.hpp"

namespace zifi {
namespace {

constexpr uint32_t kControlIdleTimeoutMs = 300000;
constexpr uint32_t kControlSendTimeoutMs = 5000;
constexpr uint32_t kDataConnectTimeoutMs = 10000;
constexpr uint32_t kDataIdleTimeoutMs = 30000;
constexpr uint32_t kStorIdleTimeoutMs = 60000;
constexpr uint32_t kControlSocketTimeoutSeconds = 5;
constexpr uint32_t kDataSocketTimeoutSeconds = 30;
constexpr uint32_t kVfsNormalTimeoutMs = 10000;
constexpr uint32_t kVfsMutateTimeoutMs = 65000;
constexpr uint32_t kVfsCloseTimeoutMs = 185000;

void setExternalError(char* error, size_t errorSize, const char* text) {
  if (error != nullptr && errorSize != 0) {
    snprintf(error, errorSize, "%s", text == nullptr ? "error" : text);
  }
}

bool copyCredential(const uint8_t* payload, uint16_t length, uint16_t& offset,
                    char* output, size_t capacity) {
  size_t used = 0;
  while (offset < length && payload[offset] != 0) {
    if (used + 1 >= capacity) {
      return false;
    }
    output[used++] = static_cast<char>(payload[offset++]);
  }
  output[used] = 0;
  if (offset < length && payload[offset] == 0) {
    ++offset;
  }
  return true;
}

size_t minimum(size_t left, size_t right) {
  return left < right ? left : right;
}

// Часы ESP идут от NTP; до синхронизации time() показывает 1970-й. Раньше
// этой отметки «сейчас» считается неизвестным.
constexpr time_t kClockValidSince = 1577836800;  // 2020-01-01
// Строка LIST показывает время для файлов моложе полугода, как «ls -l».
constexpr int64_t kListRecentSeconds = 183LL * 86400;
// Небольшой разброс часов ПК и ZX не должен переводить свежий файл в «будущий».
constexpr int64_t kListFutureSkewSeconds = 3600;
// FILEX SET_METADATA: time_mask бит 2 — время и дата изменения.
constexpr uint8_t kMetadataWriteTime = 0x04;

FatStamp writeStamp(const VfsResult& entry) {
  FatStamp stamp;
  stamp.date = entry.writeDate;
  stamp.time = entry.writeTime;
  return stamp;
}

}  // namespace

FtpServer::Session::Session()
    : passiveServer(0, 1),
      controlClient(),
      dataClient(),
      active(false),
      passiveListening(false),
      loggedIn(false),
      userAccepted(false),
      discardLine(false),
      activeEndpointSet(false),
      pendingCommand(false),
      passivePort(0),
      activePort(0),
      activeAddress(),
      lastControlActivityMs(0),
      cwd{},
      line{},
      lineLength(0),
      pendingLine{} {
  snprintf(cwd, sizeof(cwd), "/");
}

FtpServer::FtpServer(VfsBridge& bridge, EventSink eventSink,
                     void* eventContext)
    : bridge_(bridge),
      eventSink_(eventSink),
      eventContext_(eventContext),
      controlServer_(0, static_cast<uint8_t>(kMaxSessions)),
      sessions_{},
      vfsOwner_(nullptr),
      running_(false),
      port_(21),
      timezoneSeconds_(0),
      user_{},
      password_{},
      lastVfsError_{},
      ioBuffer_{},
      storEof_(false),
      storError_(false),
      storReceived_(0),
      storLastProgressMs_(0),
      ramStatCount_(0),
      ramStatAt_{},
      ramStatFree_{} {
  for (size_t index = 0; index < kMaxSessions; ++index) {
    sessions_[index].passivePort =
        kPassivePort + static_cast<uint16_t>(index);
  }
  snprintf(user_, sizeof(user_), "zx");
  snprintf(password_, sizeof(password_), "zx");
  snprintf(lastVfsError_, sizeof(lastVfsError_), "none");
}

bool FtpServer::start(const uint8_t* payload, uint16_t length,
                      uint16_t& actualPort, char* error, size_t errorSize) {
  stop();
  if (length != 0 && payload == nullptr) {
    setExternalError(error, errorSize, "ftp payload null");
    return false;
  }
  port_ = length >= 2 ? readLe16(payload) : 21;
  if (port_ == 0) {
    setExternalError(error, errorSize, "ftp port zero");
    return false;
  }

  snprintf(user_, sizeof(user_), "zx");
  snprintf(password_, sizeof(password_), "zx");
  uint16_t offset = length >= 2 ? 2 : length;
  if (offset < length) {
    char parsed[sizeof(user_)] = {};
    if (!copyCredential(payload, length, offset, parsed, sizeof(parsed))) {
      setExternalError(error, errorSize, "ftp user too long");
      return false;
    }
    if (parsed[0] != 0) {
      snprintf(user_, sizeof(user_), "%s", parsed);
    }
  }
  if (offset < length) {
    char parsed[sizeof(password_)] = {};
    if (!copyCredential(payload, length, offset, parsed, sizeof(parsed))) {
      setExternalError(error, errorSize, "ftp password too long");
      return false;
    }
    if (parsed[0] != 0) {
      snprintf(password_, sizeof(password_), "%s", parsed);
    }
  }
  if (WiFi.status() != WL_CONNECTED) {
    setExternalError(error, errorSize, "ftp no wifi");
    return false;
  }
  if (!bridge_.ready()) {
    setExternalError(error, errorSize, "ftp vfs bridge");
    return false;
  }

  controlServer_.begin(port_);
  controlServer_.setNoDelay(true);
  if (!controlServer_) {
    controlServer_.stop();
    setExternalError(error, errorSize, "ftp listen failed");
    return false;
  }
  running_ = true;
  actualPort = port_;
  setExternalError(error, errorSize, "");
  return true;
}

void FtpServer::stop() {
  vfsOwner_ = nullptr;
  for (Session& session : sessions_) {
    dropControl(session);
  }
  controlServer_.stop();
  running_ = false;
  ramStatCount_ = 0;
}

void FtpServer::sendClientState() {
  uint8_t state = 0;
  for (const Session& session : sessions_) {
    if (!session.active) {
      continue;
    }
    state = session.loggedIn ? 2 : (state == 0 ? 1 : state);
    if (state == 2) {
      break;
    }
  }
  if (eventSink_ != nullptr) {
    eventSink_(eventContext_, kEventFtpClient, &state, 1);
  }
}

void FtpServer::sendCommandEvent(const char* command, const char* argument) {
  char shown[31];
  size_t used = 0;
  while (command != nullptr && *command != 0 && used < sizeof(shown) - 1) {
    shown[used++] = *command++;
  }
  shown[used] = 0;
  if (argument != nullptr && *argument != 0 &&
      strcmp(shown, "USER") != 0 && strcmp(shown, "PASS") != 0 &&
      used < sizeof(shown) - 1) {
    shown[used++] = ' ';
    while (*argument != 0 && used < sizeof(shown) - 1) {
      shown[used++] = *argument++;
    }
  }
  shown[used] = 0;
  if (eventSink_ != nullptr) {
    eventSink_(eventContext_, kEventFtpCommand,
               reinterpret_cast<const uint8_t*>(shown),
               static_cast<uint16_t>(used));
  }
}

bool FtpServer::sendAll(WiFiClient& client, const uint8_t* data, size_t length,
                        uint32_t idleTimeoutMs) {
  size_t offset = 0;
  uint32_t idleSince = millis();
  while (offset < length) {
    const size_t written = client.write(data + offset, length - offset);
    if (written != 0) {
      offset += written;
      idleSince = millis();
      vTaskDelay(1);
      continue;
    }
    if (!client.connected() ||
        static_cast<uint32_t>(millis() - idleSince) >= idleTimeoutMs) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return true;
}

bool FtpServer::reply(Session& session, const char* text) {
  if (!session.active || text == nullptr) {
    return false;
  }
  if (!sendAll(session.controlClient, reinterpret_cast<const uint8_t*>(text),
               strlen(text), kControlSendTimeoutMs)) {
    dropControl(session, "control send");
    return false;
  }
  return true;
}

bool FtpServer::replyFormat(Session& session, const char* format, ...) {
  char text[384];
  va_list arguments;
  va_start(arguments, format);
  const int length = vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(text)) {
    return reply(session, "451 Reply is too long\r\n");
  }
  return reply(session, text);
}

void FtpServer::closePassive(Session& session) {
  session.passiveServer.stop();
  session.passiveListening = false;
}

void FtpServer::closeData(Session& session) {
  // operator bool() уже может вернуть false после FIN, хотя объект ещё держит
  // дескриптор сокета. stop() безопасен и в этом состоянии, поэтому вызываем
  // его без условия перед сбросом объекта.
  session.dataClient.stop();
  session.dataClient = WiFiClient();
}

void FtpServer::dropControl(Session& session, const char* reason) {
  (void)reason;
  const bool hadClient = session.active;
  closePassive(session);
  closeData(session);
  session.controlClient.stop();
  session.controlClient = WiFiClient();
  session.active = false;
  session.loggedIn = false;
  session.userAccepted = false;
  session.activeEndpointSet = false;
  session.pendingCommand = false;
  session.lineLength = 0;
  session.discardLine = false;
  session.pendingLine[0] = 0;
  snprintf(session.cwd, sizeof(session.cwd), "/");
  if (hadClient) {
    sendClientState();
  }
}

void FtpServer::acceptControl() {
  while (controlServer_.hasClient()) {
    WiFiClient guest = controlServer_.accept();
    if (!guest) {
      return;
    }
    Session* available = nullptr;
    for (Session& session : sessions_) {
      if (!session.active) {
        available = &session;
        break;
      }
    }
    if (available == nullptr) {
      static constexpr char kBusy[] =
          "421 Too many FTP sessions (maximum 3)\r\n";
      sendAll(guest, reinterpret_cast<const uint8_t*>(kBusy),
              sizeof(kBusy) - 1, kControlSendTimeoutMs);
      guest.stop();
      continue;
    }

    Session& session = *available;
    dropControl(session);
    session.controlClient = guest;
    session.active = true;
    session.controlClient.setNoDelay(true);
    session.controlClient.setTimeout(kControlSocketTimeoutSeconds);
    session.lineLength = 0;
    session.discardLine = false;
    session.loggedIn = false;
    session.userAccepted = false;
    session.activeEndpointSet = false;
    session.pendingCommand = false;
    session.pendingLine[0] = 0;
    snprintf(session.cwd, sizeof(session.cwd), "/");
    session.lastControlActivityMs = millis();
    sendClientState();
    replyFormat(session,
                "220 ZiFi ESP32-S3 FTP ready %s ram=%u psram=%u\r\n",
                ZIFI_BUILD_VERSION, ESP.getFreeHeap(), ESP.getFreePsram());
  }
}

bool FtpServer::commandUsesVfs(const char* line) {
  if (line == nullptr) {
    return false;
  }
  while (*line == ' ' || *line == '\t') {
    ++line;
  }
  char command[8];
  size_t used = 0;
  while (*line != 0 && *line != ' ' && *line != '\t' &&
         used + 1 < sizeof(command)) {
    command[used++] = static_cast<char>(
        toupper(static_cast<unsigned char>(*line++)));
  }
  command[used] = 0;
  static constexpr const char* kVfsCommands[] = {
      "CWD", "XCWD", "CDUP", "SIZE", "LIST", "NLST",
      "RETR", "STOR", "DELE", "MKD", "XMKD",
      "MDTM", "MFMT", "MLSD", "MLST"};
  for (const char* candidate : kVfsCommands) {
    if (strcmp(command, candidate) == 0) {
      return true;
    }
  }
  return false;
}

void FtpServer::dispatchCommand(Session& session, char* line) {
  if (!commandUsesVfs(line)) {
    executeCommand(session, line);
    return;
  }
  if (vfsOwner_ != nullptr && vfsOwner_ != &session) {
    if (session.pendingCommand) {
      reply(session, "503 Another command is already queued\r\n");
      return;
    }
    snprintf(session.pendingLine, sizeof(session.pendingLine), "%s", line);
    session.pendingCommand = true;
    return;
  }

  vfsOwner_ = &session;
  executeCommand(session, line);
  if (vfsOwner_ == &session) {
    vfsOwner_ = nullptr;
  }
  session.lastControlActivityMs = millis();
}

void FtpServer::runPendingCommand() {
  if (vfsOwner_ != nullptr) {
    return;
  }
  for (Session& session : sessions_) {
    if (!session.active || !session.pendingCommand) {
      continue;
    }
    snprintf(session.line, sizeof(session.line), "%s", session.pendingLine);
    session.pendingCommand = false;
    session.pendingLine[0] = 0;
    dispatchCommand(session, session.line);
    session.line[0] = 0;
    return;
  }
}

void FtpServer::receiveControl(Session& session) {
  if (!session.active) {
    return;
  }
  if (session.pendingCommand) {
    if (session.controlClient.available() == 0 &&
        !session.controlClient.connected()) {
      dropControl(session, "closed while queued");
    }
    return;
  }
  while (session.controlClient.available() > 0) {
    const int value = session.controlClient.read();
    if (value < 0) {
      break;
    }
    session.lastControlActivityMs = millis();
    const char byte = static_cast<char>(value);
    if (byte == '\n') {
      if (session.discardLine) {
        session.discardLine = false;
        session.lineLength = 0;
        reply(session, "500 Line too long\r\n");
        continue;
      }
      while (session.lineLength > 0 &&
             (session.line[session.lineLength - 1] == '\r' ||
              session.line[session.lineLength - 1] == ' ' ||
              session.line[session.lineLength - 1] == '\t')) {
        --session.lineLength;
      }
      session.line[session.lineLength] = 0;
      if (session.lineLength != 0) {
        dispatchCommand(session, session.line);
      }
      session.lineLength = 0;
      if (!session.active || session.pendingCommand) {
        return;
      }
      continue;
    }
    if (session.discardLine) {
      continue;
    }
    if (session.lineLength >= kMaxLine) {
      session.discardLine = true;
      session.lineLength = 0;
      continue;
    }
    session.line[session.lineLength++] = byte;
  }
  if (session.controlClient.available() == 0 &&
      !session.controlClient.connected()) {
    dropControl(session, "closed");
  } else if (!session.pendingCommand &&
             static_cast<uint32_t>(millis() - session.lastControlActivityMs) >=
             kControlIdleTimeoutMs) {
    reply(session, "421 Control connection timed out\r\n");
    dropControl(session, "idle");
  }
}

void FtpServer::serviceSessions(Session* excluded) {
  for (Session& session : sessions_) {
    if (&session != excluded) {
      receiveControl(session);
    }
  }
  // Сначала освобождаем закрывшиеся слоты, затем принимаем новые соединения:
  // быстрое переподключение клиента не должно получить ложный ответ 421.
  acceptControl();
}

void FtpServer::poll() {
  if (!running_) {
    return;
  }
  serviceSessions();
  runPendingCommand();
}

bool FtpServer::normalizePath(const Session& session, const char* argument,
                              char output[kMaxPath + 1]) const {
  if (argument == nullptr || *argument == 0) {
    snprintf(output, kMaxPath + 1, "%s", session.cwd);
    return true;
  }
  if (*argument == '/' || *argument == '\\') {
    output[0] = '/';
    output[1] = 0;
  } else {
    snprintf(output, kMaxPath + 1, "%s", session.cwd);
  }

  const char* cursor = argument;
  while (*cursor != 0) {
    while (*cursor == '/' || *cursor == '\\') {
      ++cursor;
    }
    if (*cursor == 0) {
      break;
    }
    char component[kMaxPath + 1];
    size_t componentLength = 0;
    while (*cursor != 0 && *cursor != '/' && *cursor != '\\') {
      if (componentLength >= kMaxPath) {
        return false;
      }
      component[componentLength++] = *cursor++;
    }
    component[componentLength] = 0;
    if (strcmp(component, ".") == 0 || componentLength == 0) {
      continue;
    }
    if (strcmp(component, "..") == 0) {
      size_t length = strlen(output);
      while (length > 1 && output[length - 1] != '/') {
        --length;
      }
      if (length > 1) {
        --length;
      }
      output[length] = 0;
      continue;
    }
    size_t length = strlen(output);
    const size_t slash = length > 1 ? 1 : 0;
    if (length + slash + componentLength > kMaxPath) {
      return false;
    }
    if (slash != 0) {
      output[length++] = '/';
    }
    memcpy(output + length, component, componentLength + 1);
  }
  return true;
}

bool FtpServer::parsePort(const char* argument, IPAddress& address,
                          uint16_t& port) const {
  unsigned values[6];
  char extra = 0;
  const int count = sscanf(argument, "%u,%u,%u,%u,%u,%u%c",
                           &values[0], &values[1], &values[2],
                           &values[3], &values[4], &values[5], &extra);
  if (count != 6) {
    return false;
  }
  for (unsigned value : values) {
    if (value > 255) {
      return false;
    }
  }
  const uint32_t fullPort = values[4] * 256UL + values[5];
  if (fullPort == 0) {
    return false;
  }
  address = IPAddress(values[0], values[1], values[2], values[3]);
  port = static_cast<uint16_t>(fullPort);
  return true;
}

bool FtpServer::parseEprt(const char* argument, IPAddress& address,
                          uint16_t& port) const {
  if (argument == nullptr || argument[0] == 0) {
    return false;
  }
  const char delimiter = argument[0];
  const char* familyEnd = strchr(argument + 1, delimiter);
  if (familyEnd == nullptr || familyEnd - (argument + 1) != 1 ||
      argument[1] != '1') {
    return false;
  }
  const char* addressBegin = familyEnd + 1;
  const char* addressEnd = strchr(addressBegin, delimiter);
  if (addressEnd == nullptr || addressEnd == addressBegin) {
    return false;
  }
  const char* portBegin = addressEnd + 1;
  const char* portEnd = strchr(portBegin, delimiter);
  if (portEnd == nullptr || portEnd == portBegin || portEnd[1] != 0) {
    return false;
  }
  char addressText[16];
  const size_t addressLength = static_cast<size_t>(addressEnd - addressBegin);
  if (addressLength >= sizeof(addressText)) {
    return false;
  }
  memcpy(addressText, addressBegin, addressLength);
  addressText[addressLength] = 0;
  if (!address.fromString(addressText)) {
    return false;
  }
  char portText[6];
  const size_t portLength = static_cast<size_t>(portEnd - portBegin);
  if (portLength >= sizeof(portText)) {
    return false;
  }
  memcpy(portText, portBegin, portLength);
  portText[portLength] = 0;
  char* tail = nullptr;
  const unsigned long parsed = strtoul(portText, &tail, 10);
  if (tail == portText || *tail != 0 || parsed == 0 || parsed > 65535) {
    return false;
  }
  port = static_cast<uint16_t>(parsed);
  return true;
}

void FtpServer::enterPassive(Session& session, bool extended) {
  closePassive(session);
  closeData(session);
  session.activeEndpointSet = false;
  session.passiveServer.begin(session.passivePort);
  session.passiveServer.setNoDelay(true);
  if (!session.passiveServer) {
    session.passiveServer.stop();
    reply(session, "425 Cannot enter passive mode\r\n");
    return;
  }
  session.passiveListening = true;
  if (extended) {
    replyFormat(session, "229 Entering Extended Passive Mode (|||%u|)\r\n",
                session.passivePort);
  } else {
    const IPAddress ip = WiFi.localIP();
    replyFormat(session,
                "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)\r\n",
                ip[0], ip[1], ip[2], ip[3],
                session.passivePort >> 8, session.passivePort & 0xFF);
  }
}

void FtpServer::setActive(Session& session, const char* argument,
                          bool extended) {
  closePassive(session);
  closeData(session);
  session.activeEndpointSet = false;
  IPAddress address;
  uint16_t port = 0;
  const bool valid = extended ? parseEprt(argument, address, port)
                              : parsePort(argument, address, port);
  if (!valid) {
    reply(session, "501 Bad PORT argument\r\n");
    return;
  }
  session.activeAddress = address;
  session.activePort = port;
  session.activeEndpointSet = true;
  reply(session, "200 PORT command successful\r\n");
}

bool FtpServer::openData(Session& session) {
  closeData(session);
  if (session.passiveListening) {
    const uint32_t started = millis();
    while (static_cast<uint32_t>(millis() - started) <
           kDataConnectTimeoutMs) {
      if (session.passiveServer.hasClient()) {
        session.dataClient = session.passiveServer.accept();
        break;
      }
      serviceSessions(&session);
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    closePassive(session);
    if (!session.dataClient) {
      reply(session, "425 Passive data connection timed out\r\n");
      return false;
    }
  } else if (session.activeEndpointSet) {
    const IPAddress address = session.activeAddress;
    const uint16_t port = session.activePort;
    session.activeEndpointSet = false;
    if (!session.dataClient.connect(address, port, kDataConnectTimeoutMs)) {
      closeData(session);
      reply(session, "425 Cannot open data connection\r\n");
      return false;
    }
  } else {
    reply(session, "425 Use PASV/EPSV or PORT/EPRT first\r\n");
    return false;
  }
  session.dataClient.setNoDelay(true);
  session.dataClient.setTimeout(kDataSocketTimeoutSeconds);
  return true;
}

bool FtpServer::requestVfs(VfsOperation operation, const char* path,
                           uint32_t value, VfsResult& result,
                           uint32_t timeoutMs, WaitHook hook,
                           void* hookContext) {
  memset(&result, 0, sizeof(result));
  if (!bridge_.submit(operation, path, value)) {
    snprintf(lastVfsError_, sizeof(lastVfsError_), "bridge-busy");
    return false;
  }
  return awaitVfs(operation, result, timeoutMs, hook, hookContext);
}

bool FtpServer::requestMetadata(const VfsMetadata& metadata,
                                VfsResult& result) {
  memset(&result, 0, sizeof(result));
  if (!bridge_.submitMetadata(metadata)) {
    snprintf(lastVfsError_, sizeof(lastVfsError_), "bridge-busy");
    return false;
  }
  return awaitVfs(VfsOperation::kSetMetadata, result, kVfsMutateTimeoutMs);
}

bool FtpServer::awaitVfs(VfsOperation operation, VfsResult& result,
                         uint32_t timeoutMs, WaitHook hook,
                         void* hookContext) {
  const uint32_t started = millis();
  while (static_cast<uint32_t>(millis() - started) < timeoutMs) {
    if (hook != nullptr) {
      hook(hookContext);
    }
    if (bridge_.takeResult(result)) {
      if (!result.success) {
        snprintf(lastVfsError_, sizeof(lastVfsError_), "%s", result.error);
        return false;
      }
      snprintf(lastVfsError_, sizeof(lastVfsError_), "none");
      return true;
    }
    serviceSessions(vfsOwner_);
    // До тика, но готовый ответ Z80 будит сразу: листинг каталога — это
    // обмен с мостом на каждую запись, и целый тик на каждую был заметен.
    bridge_.waitForResult(1);
  }
  snprintf(lastVfsError_, sizeof(lastVfsError_), "bridge-timeout-%u",
           static_cast<unsigned>(operation));
  return false;
}

bool FtpServer::statPath(const char* path, VfsResult& result) {
  return requestVfs(VfsOperation::kStat, path, 0, result,
                    kVfsNormalTimeoutMs);
}

bool FtpServer::resetBuffers() {
  VfsResult result;
  return requestVfs(VfsOperation::kResetBuffers, nullptr, 0, result,
                    kVfsNormalTimeoutMs);
}

void FtpServer::changeDirectory(Session& session, const char* argument) {
  char path[kMaxPath + 1];
  VfsResult result;
  if (!normalizePath(session, argument, path) || !statPath(path, result) ||
      !result.isDirectory) {
    reply(session, "550 Failed to change directory\r\n");
    return;
  }
  snprintf(session.cwd, sizeof(session.cwd), "%s", path);
  reply(session, "250 Directory changed\r\n");
}

void FtpServer::sendSize(Session& session, const char* argument) {
  char path[kMaxPath + 1];
  VfsResult result;
  if (!normalizePath(session, argument, path) || !statPath(path, result) ||
      result.isDirectory) {
    reply(session, "550 Could not get file size\r\n");
    return;
  }
  replyFormat(session, "213 %lu\r\n",
              static_cast<unsigned long>(result.size));
}

// Дата строки LIST — как у «ls -l»: «Mar 15 14:30» для файлов моложе полугода
// и «Mar 15  2024» для прочих, в том числе когда часы ESP ещё не сверены.
// Время местное — как на карте и в панели WC. Без даты (старый плагин) —
// прежнее «Jan 01 00:00».
void FtpServer::formatListDate(const VfsResult& entry, char* output,
                               size_t capacity) const {
  static const char* const kMonths[12] = {"Jan", "Feb", "Mar", "Apr",
                                          "May", "Jun", "Jul", "Aug",
                                          "Sep", "Oct", "Nov", "Dec"};
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!fatStampToCivil(writeStamp(entry), year, month, day, hour, minute,
                       second)) {
    snprintf(output, capacity, "Jan 01 00:00");
    return;
  }
  bool recent = false;
  const time_t now = time(nullptr);
  if (now >= kClockValidSince) {
    const int64_t age = static_cast<int64_t>(now) + timezoneSeconds_ -
                        civilToUnix(year, month, day, hour, minute, second);
    recent = age >= -kListFutureSkewSeconds && age < kListRecentSeconds;
  }
  if (recent) {
    snprintf(output, capacity, "%s %2d %02d:%02d", kMonths[month - 1], day,
             hour, minute);
  } else {
    snprintf(output, capacity, "%s %2d  %4d", kMonths[month - 1], day, year);
  }
}

// Факты MLSD/MLST (RFC 3659) без завершающего пробела и имени. modify — UTC;
// без даты факт опускается, клиент тогда не делает выводов о свежести.
int FtpServer::formatFacts(const VfsResult& entry, const char* name,
                           char* output, size_t capacity) const {
  const char* type = entry.isDirectory ? "dir" : "file";
  if (name != nullptr && strcmp(name, ".") == 0) {
    type = "cdir";
  } else if (name != nullptr && strcmp(name, "..") == 0) {
    type = "pdir";
  }
  int used = snprintf(output, capacity, "type=%s;", type);
  if (used < 0 || static_cast<size_t>(used) >= capacity) {
    return -1;
  }
  if (!entry.isDirectory) {
    const int size = snprintf(output + used, capacity - used, "size=%lu;",
                              static_cast<unsigned long>(entry.size));
    if (size < 0 || static_cast<size_t>(used + size) >= capacity) {
      return -1;
    }
    used += size;
  }
  int64_t modified = 0;
  if (fatStampToUnix(writeStamp(entry), timezoneSeconds_, modified)) {
    char value[15];
    formatFtpTimeVal(modified, value);
    const int modify =
        snprintf(output + used, capacity - used, "modify=%s;", value);
    if (modify < 0 || static_cast<size_t>(used + modify) >= capacity) {
      return -1;
    }
    used += modify;
  }
  return used;
}

void FtpServer::listSingle(Session& session, const char* argument) {
  char path[kMaxPath + 1];
  VfsResult result;
  if (!normalizePath(session, argument, path) || !statPath(path, result)) {
    reply(session, "550 No such file or directory\r\n");
    return;
  }
  char facts[96];
  if (formatFacts(result, nullptr, facts, sizeof(facts)) < 0) {
    reply(session, "550 No such file or directory\r\n");
    return;
  }
  replyFormat(session, "250-Listing %s\r\n %s %s\r\n250 End\r\n", path,
              facts, path);
}

void FtpServer::sendModificationTime(Session& session,
                                     const char* argument) {
  char path[kMaxPath + 1];
  VfsResult result;
  if (!normalizePath(session, argument, path) || !statPath(path, result) ||
      result.isDirectory) {
    reply(session, "550 File not found\r\n");
    return;
  }
  int64_t modified = 0;
  if (!fatStampToUnix(writeStamp(result), timezoneSeconds_, modified)) {
    reply(session, "550 Modification time not available\r\n");
    return;
  }
  char value[15];
  formatFtpTimeVal(modified, value);
  replyFormat(session, "213 %s\r\n", value);
}

// MFMT YYYYMMDDhhmmss путь — записать время изменения в запись WC (UTC в
// местное по поясу). Так FileZilla сохраняет дату закачанного файла.
void FtpServer::setModificationTime(Session& session, const char* argument) {
  int64_t requested = 0;
  const char* rest = nullptr;
  if (!parseFtpTimeVal(argument, requested, &rest) || *rest != ' ') {
    reply(session, "501 Usage: MFMT YYYYMMDDhhmmss path\r\n");
    return;
  }
  while (*rest == ' ') {
    ++rest;
  }
  char path[kMaxPath + 1];
  VfsResult result;
  if (*rest == 0 || !normalizePath(session, rest, path) ||
      !statPath(path, result)) {
    reply(session, "550 File not found\r\n");
    return;
  }
  // Прежний плагин любой ненулевой режим OPEN понимал как запись, то есть
  // удалил бы файл на OPEN=3. Метаданные в STAT отдаёт только новый плагин,
  // и только ему можно слать режим 3.
  if (!result.hasMetadata) {
    reply(session,
          "550 Setting dates needs newer ZIFIFTP.WMF and WC Improved\r\n");
    return;
  }
  FatStamp stamp;
  if (!unixToFatStamp(requested, timezoneSeconds_, stamp)) {
    reply(session, "501 Time is outside the FAT range 1980-2107\r\n");
    return;
  }
  VfsResult opened;
  if (!requestVfs(VfsOperation::kOpenRandom, path, 0, opened,
                  kVfsNormalTimeoutMs)) {
    replyFormat(session, "550 Cannot open for metadata (%s)\r\n",
                lastVfsError_);
    return;
  }
  VfsMetadata metadata;
  metadata.timeMask = kMetadataWriteTime;
  metadata.writeDate = stamp.date;
  metadata.writeTime = stamp.time;
  VfsResult applied;
  const bool set = requestMetadata(metadata, applied);
  char failure[sizeof(lastVfsError_)];
  snprintf(failure, sizeof(failure), "%s", lastVfsError_);
  VfsResult closed;
  requestVfs(VfsOperation::kCloseCommit, nullptr, 0, closed,
             kVfsCloseTimeoutMs);
  if (!set) {
    replyFormat(session, "550 Cannot set modification time (%s)\r\n",
                failure);
    return;
  }
  // FAT хранит секунды с шагом 2 — отвечаем тем, что записано на самом деле.
  int64_t stored = requested;
  fatStampToUnix(stamp, timezoneSeconds_, stored);
  char value[15];
  formatFtpTimeVal(stored, value);
  replyFormat(session, "213 Modify=%s; %s\r\n", value, rest);
}

void FtpServer::list(Session& session, ListFormat format,
                     const char* argument) {
  // LIST и NLST аргумент не разбирают: клиенты шлют туда ключи «-la». MLSD
  // по RFC 3659 принимает только путь каталога.
  char path[kMaxPath + 1];
  const char* directory = session.cwd;
  if (format == ListFormat::kMachine && argument != nullptr &&
      *argument != 0) {
    if (!normalizePath(session, argument, path)) {
      closePassive(session);
      reply(session, "550 Cannot list directory\r\n");
      return;
    }
    directory = path;
  }
  VfsResult result;
  if (!requestVfs(VfsOperation::kOpenDirectory, directory, 0, result,
                  kVfsNormalTimeoutMs)) {
    closePassive(session);
    reply(session, "550 Cannot list directory\r\n");
    return;
  }
  reply(session, "150 Here comes the directory listing\r\n");
  if (!openData(session)) {
    return;
  }

  bool failed = false;
  char failure[sizeof(lastVfsError_)] = "none";
  while (!failed) {
    if (!requestVfs(VfsOperation::kReadDirectory, nullptr, 0, result,
                    kVfsNormalTimeoutMs)) {
      snprintf(failure, sizeof(failure), "%s", lastVfsError_);
      failed = true;
      break;
    }
    if (result.atEnd) {
      break;
    }
    if (format == ListFormat::kNames) {
      failed = !sendAll(session.dataClient,
                        reinterpret_cast<const uint8_t*>(result.name),
                        strlen(result.name), kDataIdleTimeoutMs) ||
               !sendAll(session.dataClient,
                        reinterpret_cast<const uint8_t*>("\r\n"), 2,
                        kDataIdleTimeoutMs);
    } else {
      char listing[384];
      int length = 0;
      if (format == ListFormat::kLong) {
        char date[16];
        formatListDate(result, date, sizeof(date));
        length = snprintf(listing, sizeof(listing),
                          "%crwxr-xr-x 1 zx zx %lu %s %s\r\n",
                          result.isDirectory ? 'd' : '-',
                          static_cast<unsigned long>(result.size), date,
                          result.name);
      } else {
        const int facts =
            formatFacts(result, result.name, listing, sizeof(listing));
        length = facts < 0 ? facts
                           : facts + snprintf(listing + facts,
                                              sizeof(listing) -
                                                  static_cast<size_t>(facts),
                                              " %s\r\n", result.name);
      }
      failed = length < 0 || static_cast<size_t>(length) >= sizeof(listing) ||
               !sendAll(session.dataClient,
                        reinterpret_cast<const uint8_t*>(listing),
                        static_cast<size_t>(length), kDataIdleTimeoutMs);
    }
    if (failed) {
      snprintf(failure, sizeof(failure), "data-send");
    }
    serviceSessions(&session);
    vTaskDelay(1);
  }
  closeData(session);
  if (failed) {
    replyFormat(session, "426 Transfer aborted (%s)\r\n", failure);
  } else {
    reply(session, "226 Directory send OK\r\n");
  }
}

void FtpServer::retrieve(Session& session, const char* argument) {
  char path[kMaxPath + 1];
  VfsResult statResult;
  if (!normalizePath(session, argument, path) || !statPath(path, statResult) ||
      statResult.isDirectory) {
    closePassive(session);
    reply(session, "550 File not found\r\n");
    return;
  }
  if (!resetBuffers()) {
    closePassive(session);
    replyFormat(session, "451 Cannot reset transfer buffer (%s)\r\n",
                lastVfsError_);
    return;
  }

  VfsResult result;
  if (!requestVfs(VfsOperation::kOpenRead, path, 0, result,
                  kVfsNormalTimeoutMs)) {
    closePassive(session);
    reply(session, "550 Cannot open file\r\n");
    return;
  }
  reply(session, "150 Opening data connection\r\n");
  if (!openData(session)) {
    requestVfs(VfsOperation::kCloseCommit, nullptr, 0, result,
               kVfsCloseTimeoutMs);
    return;
  }

  uint32_t sent = 0;
  bool failed = false;
  char failure[sizeof(lastVfsError_)] = "none";
  while (sent < statResult.size) {
    // Пока исходящее кольцо пусто, ядро 1 читает очередной UART/VFS-блок.
    // Затем ядро 0 выгружает накопленное в TCP независимо от следующего чтения.
    if (bridge_.vfsToNetworkAvailable() == 0) {
      const uint32_t remaining = statResult.size - sent;
      const uint32_t wanted = static_cast<uint32_t>(minimum(
          static_cast<size_t>(remaining), VfsBridge::kMaxPumpPerRequest));
      if (!requestVfs(VfsOperation::kRead, nullptr, wanted, result,
                      kVfsNormalTimeoutMs)) {
        snprintf(failure, sizeof(failure), "%s", lastVfsError_);
        failed = true;
        break;
      }
      if (result.transferred == 0 || result.atEnd) {
        snprintf(failure, sizeof(failure), "early-eof");
        failed = true;
        break;
      }
    }

    const size_t ready = bridge_.vfsToNetworkAvailable();
    const size_t wanted = minimum(ready, sizeof(ioBuffer_));
    const size_t received = bridge_.readForNetwork(ioBuffer_, wanted);
    if (received == 0 ||
        !sendAll(session.dataClient, ioBuffer_, received, kDataIdleTimeoutMs)) {
      snprintf(failure, sizeof(failure), "data-send");
      failed = true;
      break;
    }
    sent += static_cast<uint32_t>(received);
    serviceSessions(&session);
  }

  closeData(session);
  const bool closed = requestVfs(VfsOperation::kCloseCommit, nullptr, 0,
                                 result, kVfsCloseTimeoutMs);
  if (!closed && !failed) {
    snprintf(failure, sizeof(failure), "%s", lastVfsError_);
    failed = true;
  }
  if (failed || sent != statResult.size) {
    replyFormat(session,
                "426 Transfer aborted (sent=%lu size=%lu %s)\r\n",
                static_cast<unsigned long>(sent),
                static_cast<unsigned long>(statResult.size), failure);
  } else {
    reply(session, "226 Transfer complete\r\n");
  }
}

bool FtpServer::prefetchStore(Session& session) {
  if (storEof_ || storError_) {
    return false;
  }
  bool progressed = false;
  while (bridge_.networkToVfsFree() != 0 &&
         session.dataClient.available() > 0) {
    const size_t free = bridge_.networkToVfsFree();
    const size_t available =
        static_cast<size_t>(session.dataClient.available());
    const size_t wanted = minimum(minimum(free, available), sizeof(ioBuffer_));
    const int received = session.dataClient.read(ioBuffer_, wanted);
    if (received <= 0) {
      storError_ = true;
      return progressed;
    }
    const size_t count = static_cast<size_t>(received);
    // writeFromNetwork — единственный записывающий поток этого кольца. Если
    // проверенный объём не поместился целиком, состояние SPSC нарушено.
    if (bridge_.writeFromNetwork(ioBuffer_, count) != count) {
      storError_ = true;
      return progressed;
    }
    storReceived_ += static_cast<uint32_t>(count);
    storLastProgressMs_ = millis();
    progressed = true;
  }
  if (session.dataClient.available() == 0 &&
      !session.dataClient.connected()) {
    storEof_ = true;
  }
  return progressed;
}

void FtpServer::prefetchStoreThunk(void* context) {
  auto* wait = static_cast<StoreWaitContext*>(context);
  wait->server->prefetchStore(*wait->session);
}

void FtpServer::store(Session& session, const char* argument) {
  char path[kMaxPath + 1];
  if (!normalizePath(session, argument, path)) {
    closePassive(session);
    reply(session, "550 Bad file name\r\n");
    return;
  }
  if (!resetBuffers()) {
    closePassive(session);
    replyFormat(session, "451 Cannot reset transfer buffer (%s)\r\n",
                lastVfsError_);
    return;
  }

  // FileZilla обычно соединяет пассивный сокет данных до STOR. Принимаем его до
  // медленного создания файла, чтобы TCP уже мог складываться в PSRAM-кольцо.
  if (!openData(session)) {
    return;
  }
  storEof_ = false;
  storError_ = false;
  storReceived_ = 0;
  storLastProgressMs_ = millis();
  ramStatCount_ = 1;
  ramStatAt_[0] = 0;
  ramStatFree_[0] = ESP.getFreeHeap();

  VfsResult result;
  StoreWaitContext wait{this, &session};
  if (!requestVfs(VfsOperation::kOpenWrite, path, 0, result,
                  kVfsMutateTimeoutMs, prefetchStoreThunk, &wait)) {
    closeData(session);
    replyFormat(session, "550 Cannot create file (%s)\r\n", lastVfsError_);
    return;
  }
  reply(session, "150 Opening data connection\r\n");

  bool failed = false;
  char failure[sizeof(lastVfsError_)] = "none";
  while (!failed) {
    prefetchStore(session);
    if (storError_) {
      snprintf(failure, sizeof(failure), "data-recv");
      failed = true;
      break;
    }

    const size_t queued = bridge_.networkToVfsAvailable();
    const size_t window = minimum(VfsBridge::kMaxPumpPerRequest,
                                  bridge_.ringCapacity());
    // При рабочей PSRAM ждём полные 16 КиБ. Хвост отправляется только после
    // настоящего EOF; fallback-кольцо на 4 КиБ естественно задаёт меньшее окно.
    if (queued >= window || (storEof_ && queued != 0)) {
      const uint32_t wanted = static_cast<uint32_t>(
          minimum(queued, VfsBridge::kMaxPumpPerRequest));
      if (!requestVfs(VfsOperation::kWrite, nullptr, wanted, result,
                      kVfsMutateTimeoutMs, prefetchStoreThunk, &wait)) {
        snprintf(failure, sizeof(failure), "%s", lastVfsError_);
        failed = true;
        break;
      }
      if (result.transferred == 0) {
        snprintf(failure, sizeof(failure), "vfs-no-progress");
        failed = true;
        break;
      }
      storLastProgressMs_ = millis();
      continue;
    }
    if (storEof_) {
      break;
    }
    if (static_cast<uint32_t>(millis() - storLastProgressMs_) >=
        kStorIdleTimeoutMs) {
      snprintf(failure, sizeof(failure), "data-timeout");
      failed = true;
      break;
    }
    serviceSessions(&session);
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  if (!failed && storReceived_ == 0) {
    snprintf(failure, sizeof(failure), "data-empty");
    failed = true;
  }

  closeData(session);
  if (failed) {
    // Отмена закрытия очищает ещё не записанный хвост PSRAM-кольца. DELETE удаляет
    // созданную запись, поэтому оборванный STOR не публикуется как успех.
    requestVfs(VfsOperation::kCloseAbort, nullptr, 0, result,
               kVfsCloseTimeoutMs);
    requestVfs(VfsOperation::kDelete, path, 0, result,
               kVfsMutateTimeoutMs);
  } else if (!requestVfs(VfsOperation::kCloseCommit, nullptr, 0, result,
                         kVfsCloseTimeoutMs)) {
    snprintf(failure, sizeof(failure), "%s", lastVfsError_);
    failed = true;
    requestVfs(VfsOperation::kDelete, path, 0, result,
               kVfsMutateTimeoutMs);
  }

  ramStatAt_[1] = storReceived_;
  ramStatFree_[1] = ESP.getFreeHeap();
  ramStatCount_ = 2;
  if (failed) {
    replyFormat(session, "426 Transfer aborted (%s bytes=%lu)\r\n", failure,
                static_cast<unsigned long>(storReceived_));
    dropControl(session, "stor failed");
  } else {
    reply(session, "226 Transfer complete\r\n");
  }
}

void FtpServer::deleteFile(Session& session, const char* argument) {
  char path[kMaxPath + 1];
  VfsResult result;
  if (normalizePath(session, argument, path) &&
      requestVfs(VfsOperation::kDelete, path, 0, result,
                 kVfsMutateTimeoutMs)) {
    reply(session, "250 File deleted\r\n");
  } else {
    reply(session, "550 Delete failed\r\n");
  }
}

void FtpServer::makeDirectory(Session& session, const char* argument) {
  if (argument == nullptr || *argument == 0) {
    reply(session, "501 Missing directory name\r\n");
    return;
  }
  char path[kMaxPath + 1];
  VfsResult result;
  if (normalizePath(session, argument, path) &&
      requestVfs(VfsOperation::kMkdir, path, 0, result,
                 kVfsMutateTimeoutMs)) {
    replyFormat(session, "257 \"%s\" created\r\n", path);
  } else {
    reply(session, "550 Cannot create directory\r\n");
  }
}

void FtpServer::executeCommand(Session& session, char* line) {
  while (*line == ' ' || *line == '\t') {
    ++line;
  }
  char* argument = line;
  while (*argument != 0 && *argument != ' ' && *argument != '\t') {
    *argument = static_cast<char>(
        toupper(static_cast<unsigned char>(*argument)));
    ++argument;
  }
  if (*argument != 0) {
    *argument++ = 0;
    while (*argument == ' ' || *argument == '\t') {
      ++argument;
    }
  }
  sendCommandEvent(line, argument);

  if (strcmp(line, "USER") == 0) {
    session.loggedIn = false;
    session.userAccepted = strcmp(argument, user_) == 0;
    sendClientState();
    reply(session, "331 Please specify the password\r\n");
    return;
  }
  if (strcmp(line, "PASS") == 0) {
    if (session.userAccepted && strcmp(argument, password_) == 0) {
      session.loggedIn = true;
      sendClientState();
      reply(session, "230 Login successful\r\n");
    } else {
      reply(session, "530 Login incorrect\r\n");
    }
    return;
  }
  if (!session.loggedIn) {
    reply(session, "530 Please login first\r\n");
    return;
  }

  if (strcmp(line, "SYST") == 0) {
    reply(session, "215 ZX Spectrum\r\n");
  } else if (strcmp(line, "FEAT") == 0) {
    reply(session,
          "211-Features:\r\n EPSV\r\n UTF8\r\n SIZE\r\n MDTM\r\n"
          " MFMT\r\n MLST type*;size*;modify*;\r\n211 End\r\n");
  } else if (strcmp(line, "OPTS") == 0 || strcmp(line, "NOOP") == 0) {
    reply(session, "200 OK\r\n");
  } else if (strcmp(line, "TYPE") == 0) {
    reply(session, "200 Switching to Binary mode\r\n");
  } else if (strcmp(line, "PWD") == 0 || strcmp(line, "XPWD") == 0) {
    replyFormat(session, "257 \"%s\"\r\n", session.cwd);
  } else if (strcmp(line, "CWD") == 0 || strcmp(line, "XCWD") == 0) {
    changeDirectory(session, argument);
  } else if (strcmp(line, "CDUP") == 0) {
    changeDirectory(session, "..");
  } else if (strcmp(line, "PORT") == 0) {
    setActive(session, argument, false);
  } else if (strcmp(line, "EPRT") == 0) {
    setActive(session, argument, true);
  } else if (strcmp(line, "PASV") == 0) {
    enterPassive(session, false);
  } else if (strcmp(line, "EPSV") == 0) {
    enterPassive(session, true);
  } else if (strcmp(line, "SIZE") == 0) {
    sendSize(session, argument);
  } else if (strcmp(line, "LIST") == 0) {
    list(session, ListFormat::kLong, argument);
  } else if (strcmp(line, "NLST") == 0) {
    list(session, ListFormat::kNames, argument);
  } else if (strcmp(line, "MLSD") == 0) {
    list(session, ListFormat::kMachine, argument);
  } else if (strcmp(line, "MLST") == 0) {
    listSingle(session, argument);
  } else if (strcmp(line, "MDTM") == 0) {
    sendModificationTime(session, argument);
  } else if (strcmp(line, "MFMT") == 0) {
    setModificationTime(session, argument);
  } else if (strcmp(line, "RETR") == 0) {
    retrieve(session, argument);
  } else if (strcmp(line, "STOR") == 0) {
    store(session, argument);
  } else if (strcmp(line, "DELE") == 0) {
    deleteFile(session, argument);
  } else if (strcmp(line, "MKD") == 0 || strcmp(line, "XMKD") == 0) {
    makeDirectory(session, argument);
  } else if (strcmp(line, "QUIT") == 0) {
    reply(session, "221 Goodbye\r\n");
    dropControl(session, "quit");
  } else {
    reply(session, "502 Command not implemented\r\n");
  }
}

size_t FtpServer::makeRamStats(uint8_t* output, size_t capacity) const {
  if (ramStatCount_ == 0) {
    return 0;
  }
  const size_t required = 1 + static_cast<size_t>(ramStatCount_) * 8;
  if (output == nullptr || capacity < required) {
    return 0;
  }
  output[0] = ramStatCount_;
  size_t offset = 1;
  for (uint8_t index = 0; index < ramStatCount_; ++index) {
    writeLe32(output + offset, ramStatAt_[index]);
    offset += 4;
    writeLe32(output + offset, ramStatFree_[index]);
    offset += 4;
  }
  return required;
}

}  // namespace zifi
