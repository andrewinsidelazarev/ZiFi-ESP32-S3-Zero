#include "zifi/wc_updater.hpp"

#include <Arduino.h>

#include <algorithm>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zifi/protocol.hpp"

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace zifi {
namespace {

// Ожидание моста дольше собственных пределов VfsClient (5 с, 60 с, 180 с):
// обмен ядра 1 всегда заканчивается раньше, и мост не остаётся занятым.
constexpr uint32_t kVfsNormalWaitMs = 15000;
constexpr uint32_t kVfsMutateWaitMs = 70000;
constexpr uint32_t kVfsCloseWaitMs = 190000;
constexpr const char* kApiHost = "api.github.com";
constexpr const char* kRawHost = "raw.githubusercontent.com";
constexpr const char* kLeftoverReason = "CHKDSK: old WCUPD.* in folder";

void* allocateLarge(size_t bytes) {
#if defined(ESP_PLATFORM)
  void* memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory != nullptr) {
    return memory;
  }
#endif
  return malloc(bytes);
}

void releaseLarge(void* memory) {
#if defined(ESP_PLATFORM)
  heap_caps_free(memory);
#else
  free(memory);
#endif
}

bool equalNoCase(const char* left, const char* right) {
  while (*left != 0 && *right != 0) {
    const unsigned char a = static_cast<unsigned char>(*left++);
    const unsigned char b = static_cast<unsigned char>(*right++);
    // FAT сравнивает имена без учёта регистра ASCII; байты UTF-8 — как есть.
    if ((a < 0x80 ? toupper(a) : a) != (b < 0x80 ? toupper(b) : b)) {
      return false;
    }
  }
  return *left == 0 && *right == 0;
}

void writeLe16(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8);
}

void writeLe24(uint8_t* output, uint32_t value) {
  // Файлы WC меньше 16 МиБ. 0xFFFFFF — «16 МиБ и больше»: плагин покажет
  // его как >16M, а не как число.
  if (value > 0xFFFFFFUL) {
    value = 0xFFFFFFUL;
  }
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8);
  output[2] = static_cast<uint8_t>(value >> 16);
}

bool safeName(const char* text, bool allowSlash) {
  if (text == nullptr || *text == 0) {
    return false;
  }
  for (const char* symbol = text; *symbol != 0; ++symbol) {
    const char value = *symbol;
    if (isalnum(static_cast<unsigned char>(value)) || value == '-' ||
        value == '_' || value == '.' || (allowSlash && value == '/')) {
      continue;
    }
    return false;
  }
  return true;
}

}  // namespace

WcUpdater::WcUpdater(VfsBridge& bridge, WcFetcher& fetcher, EventSink sink,
                     void* context)
    : bridge_(bridge),
      fetcher_(fetcher),
      sink_(sink),
      context_(context),
      stop_(false),
      diskSuspect_(false),
      listTruncated_(false),
      leftoverSeen_(false),
      files_(nullptr),
      fileCount_(0),
      repo_{},
      branch_{},
      directory_{},
      protected_{},
      protectedCount_(0),
      commit_{},
      error_{},
      directories_{},
      leftover_{},
      present_{},
      directoryCount_(0),
      statePhase_(WcPhase::kGithub),
      stateCurrent_(0),
      stateTotal_(0),
      stateText_{},
      progressTotal_(0),
      progressDone_(0),
      progressPercent_(0),
      progressSentMs_(0),
      chunk_{} {
  files_ = static_cast<WcFile*>(allocateLarge(sizeof(WcFile) * kMaxFiles));
}

WcUpdater::~WcUpdater() {
  if (files_ != nullptr) {
    releaseLarge(files_);
  }
}

bool WcUpdater::updatable(WcStatus status) {
  return status == WcStatus::kDifferent || status == WcStatus::kNew ||
         status == WcStatus::kReadError || status == WcStatus::kFailed;
}

bool WcUpdater::canUpdate(const WcFile& entry) {
  return entry.remote && updatable(entry.status) &&
         (!entry.kept || !entry.local);
}

bool WcUpdater::autoSelect(const WcFile& entry) {
  if (!canUpdate(entry)) {
    return false;
  }
  const char* dot = strrchr(entry.path, '.');
  const char* slash = strrchr(entry.path, '/');
  if (dot == nullptr || (slash != nullptr && dot < slash)) {
    return false;
  }
  return equalNoCase(dot, ".WMF") || equalNoCase(dot, ".$C") ||
         equalNoCase(dot, ".SPG");
}

const WcFile* WcUpdater::file(size_t index) const {
  return index < fileCount_ ? &files_[index] : nullptr;
}

bool WcUpdater::configure(const uint8_t* payload, uint16_t length,
                          char* error, size_t errorSize) {
  const char* fields[3 + kMaxProtected] = {};
  size_t fieldCount = 0;
  size_t start = 0;
  for (size_t index = 0; index < length; ++index) {
    if (payload[index] != 0) {
      continue;
    }
    if (index == start) {
      break;  // пустая строка — конец списка
    }
    if (fieldCount == sizeof(fields) / sizeof(fields[0])) {
      snprintf(error, errorSize, "too many fields");
      return false;
    }
    fields[fieldCount++] = reinterpret_cast<const char*>(payload + start);
    start = index + 1;
  }
  if (files_ == nullptr) {
    snprintf(error, errorSize, "no memory");
    return false;
  }
  if (fieldCount < 3 || strchr(fields[0], '/') == nullptr ||
      !safeName(fields[0], true) || !safeName(fields[1], true) ||
      !safeName(fields[2], true) ||
      strlen(fields[0]) >= sizeof(repo_) ||
      strlen(fields[1]) >= sizeof(branch_) ||
      strlen(fields[2]) >= sizeof(directory_)) {
    snprintf(error, errorSize, "bad repository");
    return false;
  }
  snprintf(repo_, sizeof(repo_), "%s", fields[0]);
  snprintf(branch_, sizeof(branch_), "%s", fields[1]);
  snprintf(directory_, sizeof(directory_), "%s", fields[2]);
  protectedCount_ = 0;
  for (size_t index = 3; index < fieldCount; ++index) {
    if (strlen(fields[index]) >= sizeof(protected_[0])) {
      snprintf(error, errorSize, "bad protected path");
      return false;
    }
    snprintf(protected_[protectedCount_++], sizeof(protected_[0]), "%s",
             fields[index]);
  }
  return true;
}

bool WcUpdater::isProtected(const char* path) const {
  for (size_t index = 0; index < protectedCount_; ++index) {
    if (equalNoCase(protected_[index], path)) {
      return true;
    }
  }
  return false;
}

WcFile* WcUpdater::findFile(const char* path) {
  for (size_t index = 0; index < fileCount_; ++index) {
    if (equalNoCase(files_[index].path, path)) {
      return &files_[index];
    }
  }
  return nullptr;
}

void WcUpdater::localPath(const char* relative, char* output,
                          size_t capacity) {
  snprintf(output, capacity, "/%s", relative);
}

bool WcUpdater::urlEncodePath(const char* path, char* output,
                              size_t capacity) {
  static const char kDigits[] = "0123456789ABCDEF";
  size_t used = 0;
  for (const unsigned char* symbol =
           reinterpret_cast<const unsigned char*>(path);
       *symbol != 0; ++symbol) {
    const unsigned char value = *symbol;
    const bool plain = isalnum(value) || value == '-' || value == '_' ||
                       value == '.' || value == '~' || value == '/';
    const size_t need = plain ? 1 : 3;
    if (used + need >= capacity) {
      return false;
    }
    if (plain) {
      output[used++] = static_cast<char>(value);
    } else {
      output[used++] = '%';
      output[used++] = kDigits[value >> 4];
      output[used++] = kDigits[value & 0x0F];
    }
  }
  output[used] = 0;
  return true;
}

void WcUpdater::fail(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(error_, sizeof(error_), format, arguments);
  va_end(arguments);
  sendState(WcPhase::kError, 0, 0, "%s", error_);
}

bool WcUpdater::sendState(WcPhase phase, uint16_t current, uint16_t total,
                          const char* format, ...) {
  statePhase_ = phase;
  stateCurrent_ = current;
  stateTotal_ = total;
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(stateText_, sizeof(stateText_), format, arguments);
  va_end(arguments);
  // [этап][текущий LE16][всего LE16][процент этапа][текст]
  uint8_t payload[kMaxEventPayload];
  payload[0] = static_cast<uint8_t>(phase);
  writeLe16(payload + 1, current);
  writeLe16(payload + 3, total);
  payload[5] = progressPercent_;
  size_t textLength = strlen(stateText_);
  if (textLength > sizeof(payload) - 6) {
    textLength = sizeof(payload) - 6;
  }
  memcpy(payload + 6, stateText_, textLength);
  progressSentMs_ = millis();
  return sink_ == nullptr ||
         sink_(context_, kEventWcuState, payload,
               static_cast<uint16_t>(6 + textLength));
}

void WcUpdater::beginProgress(uint32_t totalBytes) {
  progressTotal_ = totalBytes;
  progressDone_ = 0;
  progressPercent_ = 0;
}

void WcUpdater::addProgress(uint32_t bytes) {
  progressDone_ += bytes;
  if (progressTotal_ == 0) {
    return;
  }
  uint32_t percent = static_cast<uint32_t>(
      static_cast<uint64_t>(progressDone_) * 100U / progressTotal_);
  if (percent > 100) {
    percent = 100;
  }
  // Процент идёт и с каждым обычным событием этапа; отдельное событие ради
  // одной полосы — не чаще kProgressIntervalMs.
  const bool changed = percent != progressPercent_;
  progressPercent_ = static_cast<uint8_t>(percent);
  if (!changed ||
      static_cast<uint32_t>(millis() - progressSentMs_) < kProgressIntervalMs) {
    return;
  }
  char text[sizeof(stateText_)];
  snprintf(text, sizeof(text), "%s", stateText_);
  sendState(statePhase_, stateCurrent_, stateTotal_, "%s", text);
}

bool WcUpdater::sendEntry(size_t index) {
  const WcFile& entry = files_[index];
  uint8_t payload[kMaxEventPayload];
  payload[0] = static_cast<uint8_t>(index);
  payload[1] = static_cast<uint8_t>(entry.status);
  payload[2] = static_cast<uint8_t>((entry.remote ? 1 : 0) |
                                    (entry.local ? 2 : 0) |
                                    (entry.kept ? 4 : 0) |
                                    (canUpdate(entry) ? 8 : 0) |
                                    (autoSelect(entry) ? 16 : 0));
  writeLe24(payload + 3, entry.local ? entry.localSize : 0);
  writeLe24(payload + 6, entry.remote ? entry.remoteSize : 0);
  // Путь длиннее события — хвост: на экране важнее имя файла, а длинный путь
  // плагин всё равно показывает с '<' в начале. Срез — по границе символа
  // UTF-8, чтобы плагин не встретил оборванную последовательность.
  const char* path = entry.path;
  size_t pathLength = strlen(path);
  const size_t room = sizeof(payload) - 9;
  if (pathLength > room) {
    path += pathLength - room;
    while ((static_cast<uint8_t>(*path) & 0xC0) == 0x80) {
      ++path;
    }
    pathLength = strlen(path);
  }
  memcpy(payload + 9, path, pathLength);
  return sink_ == nullptr ||
         sink_(context_, kEventWcuEntry, payload,
               static_cast<uint16_t>(9 + pathLength));
}

bool WcUpdater::sendReady() {
  progressPercent_ = 100;
  size_t same = 0;
  size_t pending = 0;
  size_t failed = 0;
  for (size_t index = 0; index < fileCount_; ++index) {
    const WcStatus status = files_[index].status;
    if (status == WcStatus::kSame || status == WcStatus::kUpdated ||
        status == WcStatus::kKeptSame || status == WcStatus::kKeptDifferent) {
      ++same;
    }
    if (canUpdate(files_[index])) {
      ++pending;
    }
    if (status == WcStatus::kFailed) {
      ++failed;
    }
  }
  const char* cut = leftoverSeen_   ? ", CHKDSK: old WCUPD.*"
                    : listTruncated_ ? ", SD list cut"
                                     : "";
  if (failed != 0) {
    // Причина — у последнего файла, который сейчас FAILED: на экране Z80
    // другой нет. Предупреждение о карте — коротко перед ней, если причина
    // сама о нём не говорит.
    const char* reason = "";
    for (size_t index = fileCount_; index-- > 0;) {
      if (files_[index].status == WcStatus::kFailed) {
        reason = files_[index].reason;
        break;
      }
    }
    const char* note = strstr(reason, "CHKDSK") != nullptr ? ""
                       : leftoverSeen_                     ? ", CHKDSK"
                       : listTruncated_                    ? ", list cut"
                                                           : "";
    return sendState(WcPhase::kReady, static_cast<uint16_t>(pending),
                     static_cast<uint16_t>(fileCount_),
                     "%u to update, %u failed%s: %s",
                     static_cast<unsigned>(pending),
                     static_cast<unsigned>(failed), note, reason);
  }
  if (pending == 0) {
    return sendState(WcPhase::kReady, 0, static_cast<uint16_t>(fileCount_),
                     "All files match GitHub %.7s%s", commit_, cut);
  }
  return sendState(WcPhase::kReady, static_cast<uint16_t>(pending),
                   static_cast<uint16_t>(fileCount_),
                   "%u to update from GitHub %.7s%s",
                   static_cast<unsigned>(pending), commit_, cut);
}

bool WcUpdater::resend() {
  // Метка «дальше весь список, строк столько-то»: плагин считает строки
  // именно этого повтора и по его итогу видит, полон ли он. Поля
  // последнего состояния метка не трогает — оно уходит в конце.
  uint8_t marker[6];
  marker[0] = static_cast<uint8_t>(WcPhase::kSync);
  writeLe16(marker + 1, static_cast<uint16_t>(0));
  writeLe16(marker + 3, static_cast<uint16_t>(fileCount_));
  marker[5] = progressPercent_;
  if (sink_ != nullptr &&
      !sink_(context_, kEventWcuState, marker, sizeof(marker))) {
    return false;
  }
  for (size_t index = 0; index < fileCount_; ++index) {
    if (!sendEntry(index)) {
      return false;
    }
  }
  char text[sizeof(stateText_)];
  snprintf(text, sizeof(text), "%s", stateText_);
  return sendState(statePhase_, stateCurrent_, stateTotal_, "%s", text);
}

bool WcUpdater::awaitVfs(VfsResult& result, uint32_t timeoutMs) {
  const uint32_t started = millis();
  while (static_cast<uint32_t>(millis() - started) < timeoutMs) {
    if (bridge_.takeResult(result)) {
      if (!result.success) {
        snprintf(error_, sizeof(error_), "%s", result.error);
      }
      return result.success;
    }
    bridge_.waitForResult(1);
  }
  // Обмен ещё принадлежит ядру 1: забрать его результат, иначе следующий
  // запрос не пройдёт. Пределы VfsClient короче наших, так что он придёт.
  snprintf(error_, sizeof(error_), "vfs timeout");
  bridge_.reclaim(kVfsCloseWaitMs);
  return false;
}

bool WcUpdater::vfs(VfsOperation operation, const char* path, uint32_t value,
                    VfsResult& result, uint32_t timeoutMs) {
  memset(&result, 0, sizeof(result));
  if (!bridge_.submit(operation, path, value)) {
    snprintf(error_, sizeof(error_), "vfs busy");
    return false;
  }
  return awaitVfs(result, timeoutMs);
}

// Проверенная копия встаёт на место старого файла.
//
// Основной путь — FILEX MOVE_RENAME с REPLACE (Wild Commander Improved): WC
// перенаправляет запись файла на цепочку копии одной записью сектора, а при
// сбое записи возвращает прежнюю. Имя файла не пропадает ни на миг: и при
// пропадании питания на карте либо старый файл, либо новый.
//
// Защищённый файл (wc.ini) ставится переносом без REPLACE: если он всё-таки
// есть на карте, FILEX откажет (#1A EXISTS), и файл пользователя цел, даже
// если опись по ошибке сочла его отсутствующим.
WcUpdater::Replace WcUpdater::vfsReplace(const char* tempPath,
                                         const char* finalPath,
                                         const char* finalName,
                                         const char* asidePath,
                                         const char* relativeFile,
                                         bool keepExisting) {
  VfsResult result;
  memset(&result, 0, sizeof(result));
  if (!bridge_.submitMoveRename(tempPath, finalPath, false, !keepExisting)) {
    snprintf(error_, sizeof(error_), "vfs busy");
    return Replace::kUntouched;
  }
  if (awaitVfs(result, kVfsMutateWaitMs) ||
      result.status == kFilexCommittedCleanup) {
    return Replace::kDone;  // #25: перенос состоялся, хвост — потерянные кластеры
  }
  if (result.status == kMoveUnsupported) {
    return renameReplace(tempPath, finalPath, finalName, asidePath,
                         relativeFile, keepExisting);
  }
  // Отказ плагина (путь, каталог) и проверки FILEX #10..#1D — до изменений.
  if (result.status == 1 || (result.status >= 0x10 && result.status <= 0x1D)) {
    snprintf(error_, sizeof(error_), "replace refused: %s", result.error);
    return Replace::kUntouched;
  }
  // Сбой носителя (#20..#24, #26) или обрыв связи: откат мог и не пройти.
  snprintf(error_, sizeof(error_), "check the disk: replace %s", result.error);
  return Replace::kUnknown;
}

// Запасной путь для WC без FILEX MOVE. RENAME (API 74) работает внутри
// каталога и занятое имя не заменяет, поэтому прежний файл сначала
// откладывается под именем WCUPD.OLD, копия получает имя файла, и лишь потом
// отложенный удаляется.
//
// Любой отказ RENAME здесь — исход неизвестен. Даже в WC Improved код отказа
// не доказывает, что записи целы: A=0 бывает и после того, как удаление
// прежней записи легло, а откат убрал новую. Без FILEX MOVE ядро, скорее
// всего, старее, и гарантий у него меньше. Поэтому после отказа ни одна
// копия не удаляется, и запись в этом сеансе прекращается. Обе копии —
// WCUPD.OLD и WCUPD.TMP — найдёт опись следующего запуска, и каталог
// останется только для чтения до проверки карты.
WcUpdater::Replace WcUpdater::renameReplace(const char* tempPath,
                                            const char* finalPath,
                                            const char* finalName,
                                            const char* asidePath,
                                            const char* relativeFile,
                                            bool keepExisting) {
  VfsResult result;
  // Есть ли прежний файл: только ответ плагина "stat-1" — «нет». Если это
  // была ошибка чтения, а файл есть, RENAME копии на его имя откажет:
  // WC не создаёт запись с занятым именем.
  const bool exists =
      vfs(VfsOperation::kStat, finalPath, 0, result, kVfsNormalWaitMs);
  if (!exists && strcmp(result.error, "stat-1") != 0) {
    snprintf(error_, sizeof(error_), "SD: cannot stat file");
    return Replace::kUntouched;
  }
  if (exists && keepExisting) {
    snprintf(error_, sizeof(error_), "protected file is on SD");
    return Replace::kUntouched;
  }
  if (exists) {
    // Имя WCUPD.OLD должно быть свободно; остаток прошлой замены не трогаем.
    if (vfs(VfsOperation::kStat, asidePath, 0, result, kVfsNormalWaitMs)) {
      markLeftover(relativeFile);
      snprintf(error_, sizeof(error_), "%s", kLeftoverReason);
      return Replace::kUntouched;
    }
    if (strcmp(result.error, "stat-1") != 0) {
      snprintf(error_, sizeof(error_), "SD: cannot stat %s", kAsideName);
      return Replace::kUntouched;
    }
    if (!renameEntry(finalPath, kAsideName)) {
      snprintf(error_, sizeof(error_), "rename failed: CHKDSK");
      return Replace::kUnknown;
    }
  }
  if (!renameEntry(tempPath, finalName)) {
    if (exists) {
      // Попытка вернуть прежний файл на имя; исход всё равно неизвестен.
      renameEntry(asidePath, finalName);
    }
    snprintf(error_, sizeof(error_), "rename failed: CHKDSK");
    return Replace::kUnknown;
  }
  if (exists &&
      !quietVfs(VfsOperation::kDelete, asidePath, kVfsMutateWaitMs)) {
    // Прежний файл остался под именем WCUPD.OLD: следующая замена в этом
    // каталоге упёрлась бы в занятое имя — каталог только для чтения.
    markLeftover(relativeFile);
  }
  return Replace::kDone;
}

bool WcUpdater::renameEntry(const char* oldPath, const char* newName) {
  VfsResult result;
  memset(&result, 0, sizeof(result));
  if (!bridge_.submitRename(oldPath, newName, false)) {
    snprintf(error_, sizeof(error_), "vfs busy");
    return false;
  }
  return awaitVfs(result, kVfsMutateWaitMs);
}

int WcUpdater::directoryIndex(const char* dir, size_t length) const {
  for (size_t index = 0; index < directoryCount_; ++index) {
    if (strlen(directories_[index]) != length) {
      continue;
    }
    bool same = true;
    for (size_t position = 0; position < length && same; ++position) {
      const unsigned char a =
          static_cast<unsigned char>(directories_[index][position]);
      const unsigned char b = static_cast<unsigned char>(dir[position]);
      same = (a < 0x80 ? toupper(a) : a) == (b < 0x80 ? toupper(b) : b);
    }
    if (same) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

int WcUpdater::directoryOf(const char* relativeFile) const {
  const char* slash = strrchr(relativeFile, '/');
  return directoryIndex(
      relativeFile,
      slash == nullptr ? 0 : static_cast<size_t>(slash - relativeFile));
}

bool WcUpdater::leftoverIn(const char* relativeFile) const {
  const int folder = directoryOf(relativeFile);
  return folder >= 0 && leftover_[folder];
}

void WcUpdater::markLeftover(const char* relativeFile) {
  const int folder = directoryOf(relativeFile);
  if (folder >= 0) {
    leftover_[folder] = true;
  }
  leftoverSeen_ = true;
}

// Остатки прерванной замены ищутся ещё и прямым STAT: листинг мог их
// пропустить — ошибка чтения посреди каталога для WC выглядит его концом.
// Отказ STAT иной, чем «нет такого», — опись недостоверна.
bool WcUpdater::probeLeftovers(size_t index, const char* directoryPath) {
  const char* names[] = {kTempName, kAsideName};
  const size_t length = strlen(directoryPath);
  const bool slash = length != 0 && directoryPath[length - 1] == '/';
  for (const char* name : names) {
    char path[GitTreeEntry::kPathSize + 16];
    snprintf(path, sizeof(path), "%s%s%s", directoryPath, slash ? "" : "/",
             name);
    VfsResult result;
    if (vfs(VfsOperation::kStat, path, 0, result, kVfsNormalWaitMs)) {
      leftover_[index] = true;
      leftoverSeen_ = true;
    } else if (strcmp(result.error, "stat-1") != 0) {
      return false;
    }
  }
  return true;
}

bool WcUpdater::quietVfs(VfsOperation operation, const char* path,
                         uint32_t timeoutMs) {
  char saved[sizeof(error_)];
  memcpy(saved, error_, sizeof(saved));
  VfsResult result;
  const bool ok = vfs(operation, path, 0, result, timeoutMs);
  memcpy(error_, saved, sizeof(saved));
  return ok;
}

bool WcUpdater::fetchRemote() {
  if (!sendState(WcPhase::kGithub, 0, 0, "GitHub: %s", repo_)) {
    return false;
  }
  auto* json = static_cast<uint8_t*>(allocateLarge(kMaxJson + 1));
  auto* entries = static_cast<GitTreeEntry*>(
      allocateLarge(sizeof(GitTreeEntry) * (kMaxFiles + kMaxDirectories)));
  bool ok = false;
  do {
    if (json == nullptr || entries == nullptr) {
      fail("no memory for GitHub list");
      break;
    }
    char path[256];
    char encoded[128];
    size_t length = 0;
    uint16_t status = 0;
    char why[64] = {};
    snprintf(path, sizeof(path), "/repos/%s/git/ref/heads/%s", repo_, branch_);
    if (!fetcher_.get(kApiHost, path, json, kMaxJson, length, status, why,
                      sizeof(why))) {
      fail("GitHub: %s", why[0] != 0 ? why : "no answer");
      break;
    }
    if (status == 403 || status == 429) {
      fail("GitHub API limit, retry later");
      break;
    }
    if (status != 200) {
      fail("GitHub branch: HTTP %u", static_cast<unsigned>(status));
      break;
    }
    json[length] = 0;
    if (!parseGitRefCommit(reinterpret_cast<const char*>(json), length,
                           commit_)) {
      fail("GitHub branch: bad answer");
      break;
    }
    if (!urlEncodePath(directory_, encoded, sizeof(encoded))) {
      fail("bad directory");
      break;
    }
    snprintf(path, sizeof(path), "/repos/%s/git/trees/%s:%s?recursive=1",
             repo_, commit_, encoded);
    if (!sendState(WcPhase::kGithub, 0, 0, "GitHub: %.7s %s", commit_,
                   directory_) ||
        !fetcher_.get(kApiHost, path, json, kMaxJson, length, status, why,
                      sizeof(why))) {
      fail("GitHub: %s", why[0] != 0 ? why : "no answer");
      break;
    }
    if (status == 403 || status == 429) {
      fail("GitHub API limit, retry later");
      break;
    }
    if (status != 200) {
      fail("GitHub list: HTTP %u", static_cast<unsigned>(status));
      break;
    }
    size_t count = 0;
    bool truncated = true;
    if (!parseGitTree(reinterpret_cast<const char*>(json), length, entries,
                      kMaxFiles + kMaxDirectories, count, truncated) ||
        truncated) {
      fail("GitHub list: bad or too long");
      break;
    }
    fileCount_ = 0;
    directoryCount_ = 1;
    directories_[0][0] = 0;  // корень WC
    bool fits = true;
    for (size_t index = 0; index < count && fits; ++index) {
      const GitTreeEntry& entry = entries[index];
      if (entry.directory) {
        if (directoryCount_ == kMaxDirectories) {
          fits = false;
          break;
        }
        snprintf(directories_[directoryCount_++], sizeof(directories_[0]),
                 "%s", entry.path);
        continue;
      }
      if (fileCount_ == kMaxFiles) {
        fits = false;
        break;
      }
      WcFile& target = files_[fileCount_++];
      memset(&target, 0, sizeof(target));
      snprintf(target.path, sizeof(target.path), "%s", entry.path);
      memcpy(target.sha, entry.sha, sizeof(target.sha));
      target.remoteSize = entry.size;
      target.remote = true;
      target.kept = isProtected(entry.path);
      target.status = WcStatus::kUnknown;
    }
    if (!fits) {
      fail("GitHub list: too many files");
      break;
    }
    // FAT не различает регистр: два пути GitHub, одинаковые без учёта
    // регистра, обозначали бы один файл или каталог на SD. Не эталон.
    bool unique = true;
    for (size_t first = 0; first < fileCount_ && unique; ++first) {
      for (size_t second = first + 1; second < fileCount_; ++second) {
        if (equalNoCase(files_[first].path, files_[second].path)) {
          unique = false;
          break;
        }
      }
      // Файл и каталог с одним именем на FAT тоже не уживутся.
      for (size_t folder = 1; folder < directoryCount_ && unique; ++folder) {
        unique = !equalNoCase(files_[first].path, directories_[folder]);
      }
    }
    for (size_t first = 0; first < directoryCount_ && unique; ++first) {
      for (size_t second = first + 1; second < directoryCount_; ++second) {
        if (equalNoCase(directories_[first], directories_[second])) {
          unique = false;
          break;
        }
      }
    }
    if (!unique) {
      fail("GitHub list: names differ only in case");
      break;
    }
    // Служебные имена обновлятора в эталоне недопустимы: копия файла
    // совпала бы с ним самим.
    bool reserved = false;
    for (size_t index = 0; index < fileCount_ && !reserved; ++index) {
      const char* name = strrchr(files_[index].path, '/');
      name = name == nullptr ? files_[index].path : name + 1;
      reserved = equalNoCase(name, kTempName) || equalNoCase(name, kAsideName);
    }
    for (size_t folder = 1; folder < directoryCount_ && !reserved; ++folder) {
      const char* name = strrchr(directories_[folder], '/');
      name = name == nullptr ? directories_[folder] : name + 1;
      reserved = equalNoCase(name, kTempName) || equalNoCase(name, kAsideName);
    }
    if (reserved) {
      fail("GitHub list: reserved name %s", kTempName);
      break;
    }
    ok = true;
  } while (false);
  if (json != nullptr) {
    releaseLarge(json);
  }
  if (entries != nullptr) {
    releaseLarge(entries);
  }
  return ok;
}

bool WcUpdater::scanDirectory(size_t index, bool reportExtra) {
  const char* directory = directories_[index];
  char path[GitTreeEntry::kPathSize + 2];
  localPath(directory, path, sizeof(path));
  VfsResult result;
  if (!vfs(VfsOperation::kOpenDirectory, path, 0, result, kVfsNormalWaitMs)) {
    // Z80 ответил отказом ("opendir-N") — каталога на SD нет, все его файлы
    // с GitHub новые. Сбой связи отсутствием каталога не считается: иначе
    // имеющийся защищённый wc.ini стал бы «новым» и получил право на замену.
    // Отказ Z80 ещё не доказывает отсутствие: STAT, что каталог есть, —
    // значит, это ошибка чтения, и опись недостоверна.
    // Отказ плагина — статус 1; иное (короткий ответ) — не доказательство.
    if (strcmp(result.error, "opendir-1") == 0) {
      VfsResult stat;
      if (!vfs(VfsOperation::kStat, path, 0, stat, kVfsNormalWaitMs) &&
          strcmp(stat.error, "stat-1") == 0) {
        return true;
      }
    }
    fail("SD: cannot read %s", path);
    return false;
  }
  present_[index] = true;
  for (;;) {
    if (stop_) {
      return false;
    }
    if (!vfs(VfsOperation::kReadDirectory, nullptr, 0, result,
             kVfsNormalWaitMs)) {
      fail("SD: cannot read %s", path);
      return false;
    }
    if (result.atEnd) {
      if (!probeLeftovers(index, path)) {
        fail("SD: cannot read %s", path);
        return false;
      }
      return true;
    }
    if (result.isDirectory || strcmp(result.name, ".") == 0 ||
        strcmp(result.name, "..") == 0) {
      continue;
    }
    // Остаток прерванной замены в список не входит и не удаляется: запись в
    // этот каталог запрещена до проверки карты.
    if (equalNoCase(result.name, kTempName) ||
        equalNoCase(result.name, kAsideName)) {
      leftover_[index] = true;
      leftoverSeen_ = true;
      continue;
    }
    char relative[GitTreeEntry::kPathSize];
    if (directory[0] == 0) {
      snprintf(relative, sizeof(relative), "%s", result.name);
    } else if (snprintf(relative, sizeof(relative), "%s/%s", directory,
                        result.name) >= static_cast<int>(sizeof(relative))) {
      listTruncated_ = listTruncated_ || reportExtra;
      continue;
    }
    WcFile* known = findFile(relative);
    if (known != nullptr) {
      known->local = true;
      known->localSize = result.size;
      continue;
    }
    // В корне SD лежит что угодно; чужие файлы показываем только в каталогах
    // самого WC — там это, например, плагины ZiFi.
    if (!reportExtra) {
      continue;
    }
    if (fileCount_ == kMaxFiles) {
      listTruncated_ = true;
      continue;
    }
    WcFile& extra = files_[fileCount_++];
    memset(&extra, 0, sizeof(extra));
    snprintf(extra.path, sizeof(extra.path), "%s", relative);
    extra.local = true;
    extra.localSize = result.size;
    extra.kept = isProtected(relative);
    extra.status = WcStatus::kLocalOnly;
  }
}

// Группа строки в списке: 0 — сам Wild Commander (загрузчик *.$C в корне
// карты), 1 — плагины *.WMF (и чужие, что есть только на SD), 2 — всё
// остальное.
static int listGroup(const WcFile& file) {
  const char* slash = strrchr(file.path, '/');
  const char* name = slash == nullptr ? file.path : slash + 1;
  const char* dot = strrchr(name, '.');
  if (dot != nullptr && slash == nullptr && equalNoCase(dot, ".$C")) {
    return 0;
  }
  if (dot != nullptr && equalNoCase(dot, ".WMF")) {
    return 1;
  }
  return 2;
}

// Путь без учёта регистра ASCII, как его видит FAT; байты UTF-8 — как есть.
static bool pathBefore(const char* left, const char* right) {
  for (;; ++left, ++right) {
    const unsigned char a = static_cast<unsigned char>(*left);
    const unsigned char b = static_cast<unsigned char>(*right);
    const int ua = a < 0x80 ? toupper(a) : a;
    const int ub = b < 0x80 ? toupper(b) : b;
    if (ua != ub || a == 0) {
      return ua < ub;
    }
  }
}

// Порядок списка: сам WC, затем плагины по алфавиту, затем всё остальное по
// алфавиту. Сортируется до сверки: строки уходят плагину по номерам в этом
// порядке, и те же номера возвращаются в APPLY.
void WcUpdater::sortFiles() {
  std::stable_sort(files_, files_ + fileCount_,
                   [](const WcFile& left, const WcFile& right) {
                     const int a = listGroup(left);
                     const int b = listGroup(right);
                     if (a != b) {
                       return a < b;
                     }
                     return pathBefore(left.path, right.path);
                   });
}

bool WcUpdater::scanLocal() {
  if (!sendState(WcPhase::kLocal, 0, 0, "SD: reading WC folders")) {
    return false;
  }
  // Опись по каталогам, известным GitHub. Записи GitHub уже в таблице, а
  // файлы, которых на GitHub нет, добавляются в её конец.
  for (size_t index = 0; index < directoryCount_; ++index) {
    if (!scanDirectory(index, directories_[index][0] != 0)) {
      return false;
    }
  }
  return true;
}

bool WcUpdater::hashLocal(const char* path, uint32_t size,
                          uint8_t digest[Sha1::kDigestSize]) {
  VfsResult result;
  // SHA считается по длине с GitHub, поэтому сначала — длина самого файла:
  // верное начало с лишним хвостом не должно сойти за целый файл.
  if (!vfs(VfsOperation::kStat, path, 0, result, kVfsNormalWaitMs)) {
    return false;
  }
  if (result.isDirectory || result.size != size) {
    snprintf(error_, sizeof(error_), "SD size %lu, expected %lu",
             static_cast<unsigned long>(result.size),
             static_cast<unsigned long>(size));
    return false;
  }
  if (!vfs(VfsOperation::kResetBuffers, nullptr, 0, result,
           kVfsNormalWaitMs) ||
      !vfs(VfsOperation::kOpenRead, path, 0, result, kVfsNormalWaitMs)) {
    return false;
  }
  Sha1 sha;
  gitBlobBegin(sha, size);
  uint32_t done = 0;
  bool ok = true;
  while (done < size) {
    if (stop_) {
      ok = false;
      break;
    }
    if (bridge_.vfsToNetworkAvailable() == 0) {
      const uint32_t wanted = static_cast<uint32_t>(
          size - done < VfsBridge::kMaxPumpPerRequest
              ? size - done
              : VfsBridge::kMaxPumpPerRequest);
      if (!vfs(VfsOperation::kRead, nullptr, wanted, result,
               kVfsNormalWaitMs) ||
          result.transferred == 0) {
        ok = false;
        break;
      }
    }
    const size_t available = bridge_.vfsToNetworkAvailable();
    const size_t wanted = available < sizeof(chunk_) ? available
                                                     : sizeof(chunk_);
    const size_t received = bridge_.readForNetwork(chunk_, wanted);
    if (received == 0 || done + received > size) {
      ok = false;
      break;
    }
    sha.update(chunk_, received);
    done += static_cast<uint32_t>(received);
    addProgress(static_cast<uint32_t>(received));
  }
  // После сбоя чтения закрытие — уборка: его отказ не подменяет причину.
  VfsResult closed;
  const bool closeOk =
      ok ? vfs(VfsOperation::kCloseCommit, nullptr, 0, closed, kVfsCloseWaitMs)
         : quietVfs(VfsOperation::kCloseCommit, nullptr, kVfsCloseWaitMs);
  if (!ok || !closeOk) {
    return false;
  }
  sha.finish(digest);
  return true;
}

bool WcUpdater::hashFiles() {
  size_t total = 0;
  uint32_t bytes = 0;
  for (size_t index = 0; index < fileCount_; ++index) {
    if (files_[index].remote && files_[index].local) {
      ++total;
      if (files_[index].localSize == files_[index].remoteSize) {
        bytes += files_[index].localSize;
      }
    }
  }
  beginProgress(bytes);
  size_t current = 0;
  for (size_t index = 0; index < fileCount_; ++index) {
    WcFile& entry = files_[index];
    if (stop_) {
      return false;
    }
    if (!entry.remote) {
      entry.status = WcStatus::kLocalOnly;
    } else if (!entry.local) {
      // Даже защищённый файл, которого на SD нет, ставится с GitHub: без
      // него WC не запустится. Заменять имеющийся защищённый файл нельзя.
      entry.status = WcStatus::kNew;
    } else {
      ++current;
      if (!sendState(WcPhase::kCheck, static_cast<uint16_t>(current),
                     static_cast<uint16_t>(total), "SHA %s", entry.path)) {
        return false;
      }
      bool same = false;
      bool readable = true;
      if (entry.localSize == entry.remoteSize) {
        char path[GitTreeEntry::kPathSize + 2];
        localPath(entry.path, path, sizeof(path));
        uint8_t digest[Sha1::kDigestSize];
        readable = hashLocal(path, entry.localSize, digest);
        same = readable && memcmp(digest, entry.sha, sizeof(digest)) == 0;
      }
      if (stop_) {
        return false;  // чтение прервано: итог сверки недостоверен
      }
      if (entry.kept) {
        entry.status = same ? WcStatus::kKeptSame : WcStatus::kKeptDifferent;
      } else if (!readable) {
        entry.status = WcStatus::kReadError;
      } else {
        entry.status = same ? WcStatus::kSame : WcStatus::kDifferent;
      }
    }
    // Строка уходит сразу: список на экране растёт по ходу проверки.
    if (!sendEntry(index)) {
      return false;
    }
  }
  return true;
}

bool WcUpdater::check() {
  fileCount_ = 0;
  if (!fetchRemote() || !scanLocal()) {
    char reason[sizeof(error_)];
    snprintf(reason, sizeof(reason), "%s",
             stop_ ? "stopped" : (error_[0] != 0 ? error_ : "check failed"));
    fail("%s", reason);
    return false;
  }
  sortFiles();
  if (!hashFiles()) {
    char reason[sizeof(error_)];
    snprintf(reason, sizeof(reason), "%s",
             stop_ ? "stopped" : (error_[0] != 0 ? error_ : "check failed"));
    fail("%s", reason);
    return false;
  }
  return sendReady();
}

bool WcUpdater::download(const WcFile& file, uint8_t* buffer) {
  char encoded[GitTreeEntry::kPathSize * 3];
  char directory[sizeof(directory_) * 3];
  char path[sizeof(encoded) + sizeof(directory) + 160];
  if (!urlEncodePath(file.path, encoded, sizeof(encoded)) ||
      !urlEncodePath(directory_, directory, sizeof(directory))) {
    snprintf(error_, sizeof(error_), "path too long");
    return false;
  }
  snprintf(path, sizeof(path), "/%s/%s/%s/%s", repo_, commit_, directory,
           encoded);
  for (unsigned attempt = 1; attempt <= kDownloadAttempts; ++attempt) {
    if (stop_) {
      return false;
    }
    size_t length = 0;
    uint16_t status = 0;
    char why[64] = {};
    // Буфер на байт больше файла: лишний байт в ответе — уже не тот файл.
    if (!fetcher_.get(kRawHost, path, buffer, file.remoteSize + 1, length,
                      status, why, sizeof(why))) {
      snprintf(error_, sizeof(error_), "download: %s", why);
      continue;
    }
    if (status != 200 || length != file.remoteSize) {
      snprintf(error_, sizeof(error_), "download: HTTP %u, %lu bytes",
               static_cast<unsigned>(status),
               static_cast<unsigned long>(length));
      continue;
    }
    Sha1 sha;
    gitBlobBegin(sha, file.remoteSize);
    sha.update(buffer, length);
    uint8_t digest[Sha1::kDigestSize];
    sha.finish(digest);
    if (memcmp(digest, file.sha, sizeof(digest)) == 0) {
      return true;
    }
    snprintf(error_, sizeof(error_), "download: SHA mismatch");
  }
  return false;
}

bool WcUpdater::writeLocal(const char* path, const uint8_t* data,
                           uint32_t size, bool& left) {
  left = false;
  VfsResult result;
  if (!vfs(VfsOperation::kResetBuffers, nullptr, 0, result,
           kVfsNormalWaitMs) ||
      !vfs(VfsOperation::kOpenWrite, path, 0, result, kVfsMutateWaitMs)) {
    // Файл не открыт: созданного здесь нет, а чужой с этим именем плагин
    // не тронул — удалять нечего.
    return false;
  }
  left = true;
  uint32_t offset = 0;
  bool ok = true;
  const size_t window = VfsBridge::kMaxPumpPerRequest < bridge_.ringCapacity()
                            ? VfsBridge::kMaxPumpPerRequest
                            : bridge_.ringCapacity();
  while (ok && (offset < size || bridge_.networkToVfsAvailable() != 0)) {
    if (stop_) {
      ok = false;
      break;
    }
    while (offset < size && bridge_.networkToVfsFree() != 0) {
      const size_t free = bridge_.networkToVfsFree();
      const size_t part = size - offset < free ? size - offset : free;
      const size_t written = bridge_.writeFromNetwork(data + offset, part);
      if (written == 0) {
        break;
      }
      offset += static_cast<uint32_t>(written);
    }
    const size_t queued = bridge_.networkToVfsAvailable();
    if (queued < window && offset < size) {
      continue;  // кольцо ещё не набрало окно: докладываем данные
    }
    const uint32_t wanted = static_cast<uint32_t>(
        queued < VfsBridge::kMaxPumpPerRequest
            ? queued
            : VfsBridge::kMaxPumpPerRequest);
    if (!vfs(VfsOperation::kWrite, nullptr, wanted, result,
             kVfsMutateWaitMs) ||
        result.transferred == 0) {
      ok = false;
    } else {
      addProgress(result.transferred);
    }
  }
  if (ok && vfs(VfsOperation::kCloseCommit, nullptr, 0, result,
                kVfsCloseWaitMs)) {
    return true;
  }
  // Недописанная копия — своя (её создал OPEN выше): убрать. Отказы уборки
  // не подменяют причину сбоя в error_.
  quietVfs(VfsOperation::kCloseAbort, nullptr, kVfsCloseWaitMs);
  left = !quietVfs(VfsOperation::kDelete, path, kVfsMutateWaitMs);
  return false;
}

bool WcUpdater::ensureDirectory(const char* relativeFile) {
  char partial[GitTreeEntry::kPathSize + 2];
  const char* slash = strchr(relativeFile, '/');
  while (slash != nullptr) {
    const size_t length = static_cast<size_t>(slash - relativeFile);
    snprintf(partial, sizeof(partial), "/%.*s", static_cast<int>(length),
             relativeFile);
    const int folder = directoryIndex(relativeFile, length);
    VfsResult result;
    if (vfs(VfsOperation::kStat, partial, 0, result, kVfsNormalWaitMs)) {
      if (!result.isDirectory) {
        snprintf(error_, sizeof(error_), "%s is a file", partial);
        return false;
      }
      // Опись сочла каталог отсутствующим, а он есть: чтение карты сбоит, и
      // остаток прошлой замены в нём мог остаться незамеченным. Писать нельзя.
      if (folder >= 0 && !present_[folder]) {
        diskSuspect_ = true;
        snprintf(error_, sizeof(error_), "SD read errors: CHKDSK");
        return false;
      }
    } else if (!vfs(VfsOperation::kMkdir, partial, 0, result,
                    kVfsMutateWaitMs)) {
      snprintf(error_, sizeof(error_), "cannot create %s", partial);
      return false;
    } else if (folder >= 0) {
      present_[folder] = true;  // создан сейчас: остатков в нём нет
    }
    slash = strchr(slash + 1, '/');
  }
  return true;
}

bool WcUpdater::updateFile(size_t index) {
  WcFile& entry = files_[index];
  entry.reason[0] = 0;
  const char* refuse = nullptr;
  if (diskSuspect_) {
    refuse = "check the disk first";
  } else if (entry.remoteSize > kMaxFileSize) {
    refuse = "file too big for ESP";
  } else if (leftoverIn(entry.path)) {
    refuse = kLeftoverReason;
  }
  auto* buffer = refuse != nullptr
                     ? nullptr
                     : static_cast<uint8_t*>(allocateLarge(
                           static_cast<size_t>(entry.remoteSize) + 1));
  if (refuse == nullptr && buffer == nullptr) {
    refuse = "no memory for file";
  }
  if (refuse != nullptr) {
    snprintf(entry.reason, sizeof(entry.reason), "%s", refuse);
    entry.status = WcStatus::kFailed;
    return false;
  }
  char finalPath[GitTreeEntry::kPathSize + 2];
  char tempPath[GitTreeEntry::kPathSize + 16];
  char asidePath[GitTreeEntry::kPathSize + 16];
  localPath(entry.path, finalPath, sizeof(finalPath));
  const char* lastSlash = strrchr(entry.path, '/');
  if (lastSlash == nullptr) {
    snprintf(tempPath, sizeof(tempPath), "/%s", kTempName);
    snprintf(asidePath, sizeof(asidePath), "/%s", kAsideName);
  } else {
    const int folder = static_cast<int>(lastSlash - entry.path);
    snprintf(tempPath, sizeof(tempPath), "/%.*s/%s", folder, entry.path,
             kTempName);
    snprintf(asidePath, sizeof(asidePath), "/%.*s/%s", folder, entry.path,
             kAsideName);
  }
  const char* finalName = lastSlash == nullptr ? entry.path : lastSlash + 1;

  // Причина неудачи запоминается сразу, до уборки: отказ удаления копии её
  // не подменит.
  char reason[sizeof(entry.reason)] = {};
  auto note = [this, &reason](const char* text) {
    snprintf(reason, sizeof(reason), "%s", stop_ ? "stopped" : text);
  };
  auto errorOr = [this](const char* fallback) -> const char* {
    return error_[0] != 0 ? error_ : fallback;
  };

  bool done = false;
  bool installed = false;  // копия встала на имя файла в этом вызове
  bool keptThere = false;  // защищённый файл уже на SD: только сверка
  // Два полных круга: если после замены файл на SD не сошёлся, он
  // скачивается и пишется заново.
  for (unsigned round = 0; round < 2 && !done && !stop_; ++round) {
    if (entry.kept) {
      // Защищённый файл ставится, только если его нет. Есть — значит, он
      // уже стоит (прошлый круг поставил его, а проверка чтением отказала,
      // или опись его не увидела): только сверить, не перезаписывая.
      VfsResult stat;
      if (vfs(VfsOperation::kStat, finalPath, 0, stat, kVfsNormalWaitMs)) {
        keptThere = true;
        entry.local = true;
        entry.localSize = stat.size;
        uint8_t digest[Sha1::kDigestSize];
        done = !stat.isDirectory && stat.size == entry.remoteSize &&
               hashLocal(finalPath, entry.remoteSize, digest) &&
               memcmp(digest, entry.sha, sizeof(digest)) == 0;
        break;
      }
      if (strcmp(stat.error, "stat-1") != 0) {
        note("SD: cannot stat file");
        break;
      }
    }
    // Остаток мог появиться и на прошлом круге (не удалилась копия).
    if (leftoverIn(entry.path)) {
      note(kLeftoverReason);
      break;
    }
    error_[0] = 0;
    if (!download(entry, buffer) || !ensureDirectory(entry.path)) {
      note(errorOr("download failed"));
      break;
    }
    // Имя копии должно быть свободно. Остаток прошлой замены не удаляется
    // (он может делить цепочку с файлом): каталог — только для чтения.
    VfsResult probe;
    if (vfs(VfsOperation::kStat, tempPath, 0, probe, kVfsNormalWaitMs)) {
      markLeftover(entry.path);
      note(kLeftoverReason);
      break;
    }
    if (strcmp(probe.error, "stat-1") != 0) {
      note("SD: cannot stat copy");
      break;
    }
    bool staged = false;
    bool ours = false;  // на SD лежит копия, созданная здесь
    for (unsigned attempt = 1; attempt <= kWriteAttempts && !stop_;
         ++attempt) {
      error_[0] = 0;
      if (!writeLocal(tempPath, buffer, entry.remoteSize, ours)) {
        // OPEN отказал, хотя STAT имени не нашёл: оно занято (ошибка чтения
        // скрыла остаток) либо каталог не пишется — копию не создать.
        note(strncmp(error_, "open-", 5) == 0
                 ? "cannot create WCUPD.TMP: CHKDSK"
                 : errorOr("SD copy: write failed"));
        if (ours) {
          break;  // своя недописанная копия не удалилась
        }
        continue;
      }
      uint8_t digest[Sha1::kDigestSize];
      error_[0] = 0;
      if (!hashLocal(tempPath, entry.remoteSize, digest)) {
        note(errorOr("SD copy: read failed"));
      } else if (memcmp(digest, entry.sha, sizeof(digest)) != 0) {
        note("SD copy: SHA mismatch");
      } else {
        staged = true;
        break;
      }
      // Копия своя (занятое имя OPEN на запись не берёт): её можно удалить.
      if (!quietVfs(VfsOperation::kDelete, tempPath, kVfsMutateWaitMs)) {
        break;
      }
      ours = false;
    }
    if (!staged) {
      if (ours) {
        markLeftover(entry.path);  // своя копия осталась: только чтение
      }
      break;
    }
    error_[0] = 0;
    const Replace replaced = vfsReplace(tempPath, finalPath, finalName,
                                        asidePath, entry.path, entry.kept);
    if (replaced == Replace::kUntouched) {
      note(errorOr("replace refused"));
      if (!quietVfs(VfsOperation::kDelete, tempPath, kVfsMutateWaitMs)) {
        markLeftover(entry.path);
        break;
      }
      if (entry.kept) {
        continue;  // защищённый файл мог найтись на SD: круг его сверит
      }
      break;
    }
    installed = installed || replaced == Replace::kDone;
    const bool unknown = replaced == Replace::kUnknown;
    if (unknown) {
      note(errorOr("replace failed: CHKDSK"));
    }
    // Итог — по самой карте, и при неизвестном исходе тоже: файл мог встать.
    uint8_t digest[Sha1::kDigestSize];
    error_[0] = 0;
    const bool read = hashLocal(finalPath, entry.remoteSize, digest);
    const bool good = read && memcmp(digest, entry.sha, sizeof(digest)) == 0;
    if (unknown) {
      // Верный файл на месте засчитывается, но копии не трогаем и в этом
      // сеансе больше не пишем — нужна проверка карты.
      done = good;
      diskSuspect_ = true;
      break;
    }
    if (good) {
      done = true;
    } else {
      note(read ? "SD file: SHA mismatch" : errorOr("SD file: read failed"));
    }
  }
  releaseLarge(buffer);
  if (!done && reason[0] == 0) {
    snprintf(reason, sizeof(reason), "%s", stop_ ? "stopped" : "not updated");
  }
  // Причина хранится и при успехе с сомнением: её покажет ERROR.
  snprintf(entry.reason, sizeof(entry.reason), "%s", reason);
  if (done) {
    entry.status = keptThere && !installed ? WcStatus::kKeptSame
                                           : WcStatus::kUpdated;
    entry.local = true;
    entry.localSize = entry.remoteSize;
  } else if (keptThere) {
    entry.status = WcStatus::kKeptDifferent;  // файл пользователя не трогаем
  } else {
    entry.status = WcStatus::kFailed;
  }
  return done;
}

bool WcUpdater::apply(const uint8_t* indices, size_t count) {
  if (diskSuspect_) {
    fail("check the disk first");
    return false;
  }
  size_t total = 0;
  uint32_t bytes = 0;
  for (size_t position = 0; position < count; ++position) {
    const size_t index = indices[position];
    if (index < fileCount_ && canUpdate(files_[index])) {
      ++total;
      bytes += 3U * files_[index].remoteSize;
    }
  }
  beginProgress(bytes);
  size_t current = 0;
  for (size_t position = 0; position < count && !stop_; ++position) {
    const size_t index = indices[position];
    if (index >= fileCount_ || !canUpdate(files_[index])) {
      continue;
    }
    ++current;
    if (!sendState(WcPhase::kApply, static_cast<uint16_t>(current),
                   static_cast<uint16_t>(total), "Update %s",
                   files_[index].path)) {
      return false;
    }
    updateFile(index);
    if (!sendEntry(index)) {
      return false;
    }
    if (diskSuspect_) {
      // Исход замены неизвестен или опись недостоверна: остальные файлы не
      // трогаем.
      fail("%s", files_[index].reason[0] != 0 ? files_[index].reason
                                              : "check the disk");
      return false;
    }
  }
  if (stop_) {
    snprintf(error_, sizeof(error_), "stopped");
    return false;
  }
  return sendReady();
}

}  // namespace zifi
