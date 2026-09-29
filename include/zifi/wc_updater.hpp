#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/git_sha1.hpp"
#include "zifi/github_tree.hpp"
#include "zifi/vfs_bridge.hpp"

namespace zifi {

// HTTPS GET для обновлятора WC: всё тело — в буфер вызывающего.
// status — код ответа HTTP; тело длиннее capacity — отказ.
class WcFetcher {
 public:
  virtual ~WcFetcher() = default;
  virtual bool get(const char* host, const char* path, uint8_t* buffer,
                   size_t capacity, size_t& length, uint16_t& status,
                   char* error, size_t errorSize) = 0;
};

// Состояние файла WC. Значения идут в событии kEventWcuEntry как есть —
// плагин держит ту же таблицу.
enum class WcStatus : uint8_t {
  kUnknown = 0,        // ещё не проверен
  kSame = 1,           // SHA копии на SD совпал с GitHub
  kDifferent = 2,      // SHA (или длина) отличается
  kNew = 3,            // есть на GitHub, нет на SD
  kLocalOnly = 4,      // только на SD, в каталоге WC (чужой плагин и т. п.)
  kKeptSame = 5,       // защищённый файл (wc.ini) совпал
  kKeptDifferent = 6,  // защищённый файл отличается — его не трогаем
  kReadError = 7,      // файл на SD не читается
  kUpdated = 8,        // обновлён в этом сеансе: SHA на SD совпал с GitHub
  kFailed = 9,         // обновить не удалось, прежний файл цел
};

// Этап в событии kEventWcuState.
enum class WcPhase : uint8_t {
  kGithub = 1,  // чтение списка с GitHub
  kLocal = 2,   // опись каталогов на SD
  kCheck = 3,   // SHA файлов на SD, current/total
  kReady = 4,   // список готов, ждём команды; current — к обновлению
  kApply = 5,   // обновление, current/total
  // Метка WCU_SYNC: дальше весь список заново, total — число строк; за
  // строками — последнее состояние. Поля последнего состояния она не меняет.
  kSync = 6,
  kError = 0xFF,
};

struct WcFile {
  // Путь относительно корня WC на SD и каталога exe/ на GitHub.
  char path[GitTreeEntry::kPathSize];
  uint8_t sha[Sha1::kDigestSize];
  uint32_t remoteSize;
  uint32_t localSize;
  bool remote;
  bool local;
  bool kept;
  WcStatus status;
  // Причина последней неудачи (или сомнения) — для строки итога.
  char reason[40];
};

// Проверка и восстановление Wild Commander по каталогу exe/ на GitHub —
// аналог sfc /scannow. Эталон — SHA-1 git-объектов из GitHub API; каждый файл
// на SD читается целиком и хэшируется тем же способом. Обновление: скачать,
// сверить SHA, записать во временный файл, прочитать обратно и сверить, лишь
// затем заменить старый. Любой сбой оставляет прежний файл нетронутым.
// Все методы, кроме requestStop, вызываются одной задачей ядра 0.
class WcUpdater {
 public:
  static constexpr size_t kMaxFiles = 96;
  static constexpr size_t kMaxProtected = 8;
  static constexpr size_t kMaxJson = 64 * 1024;
  static constexpr size_t kMaxFileSize = 1024 * 1024;
  static constexpr unsigned kDownloadAttempts = 3;
  static constexpr unsigned kWriteAttempts = 3;
  static constexpr const char* kTempName = "WCUPD.TMP";
  // Прежний файл на время замены запасным путём (WC без FILEX MOVE).
  static constexpr const char* kAsideName = "WCUPD.OLD";
  static constexpr unsigned kRenameAttempts = 2;
  // Ответы Z80 на VFS MOVE_RENAME: у WC нет FILEX MOVE (ничего не менялось)
  // и статус FILEX «перенос состоялся, старая цепочка не освобождена».
  static constexpr uint8_t kMoveUnsupported = 0xFE;
  static constexpr uint8_t kFilexCommittedCleanup = 0x25;
  static constexpr uint32_t kProgressIntervalMs = 250;

  using EventSink = bool (*)(void* context, uint8_t command,
                             const uint8_t* data, uint16_t length);

  WcUpdater(VfsBridge& bridge, WcFetcher& fetcher, EventSink sink,
            void* context);
  ~WcUpdater();

  WcUpdater(const WcUpdater&) = delete;
  WcUpdater& operator=(const WcUpdater&) = delete;

  // [repo]\0[ветка]\0[каталог на GitHub]\0[защищённые пути]\0...\0
  bool configure(const uint8_t* payload, uint16_t length, char* error,
                 size_t errorSize);
  // Список GitHub, опись SD, SHA каждого общего файла. События: состояние
  // и по записи на файл, в конце — kReady.
  bool check();
  // Обновить файлы с указанными номерами. Неподходящие номера пропускаются.
  bool apply(const uint8_t* indices, size_t count);

  // Остановка — навсегда для этого сеанса: следующий APPLY её не снимает.
  void requestStop() { stop_ = true; }
  bool stopRequested() const { return stop_; }
  const volatile bool* stopFlag() const { return &stop_; }
  // Все строки списка и последнее состояние заново (WCU_SYNC).
  bool resend();

  size_t fileCount() const { return fileCount_; }
  const WcFile* file(size_t index) const;
  const char* commit() const { return commit_; }
  const char* lastError() const { return error_; }

  static bool updatable(WcStatus status);
  // Можно ли обновлять файл: он есть на GitHub, отличается или отсутствует,
  // а защищённый — только если его на SD нет вовсе.
  static bool canUpdate(const WcFile& entry);
  // Отметка по клавише A: только обновляемые исполняемые файлы (.WMF, .$C,
  // .spg). Тексты, лицензии и меню пользователь отмечает сам.
  static bool autoSelect(const WcFile& entry);

 private:
  bool fetchRemote();
  bool scanLocal();
  void sortFiles();
  bool scanDirectory(size_t index, bool reportExtra);
  bool hashFiles();
  bool updateFile(size_t index);
  bool download(const WcFile& file, uint8_t* buffer);
  // left — на SD осталась копия, созданная этим вызовом: при успехе либо
  // при сбое, когда удалить её не удалось. Занятое имя плагин обновлятора
  // на запись не открывает (MKFILE без удаления прежнего файла).
  bool writeLocal(const char* path, const uint8_t* data, uint32_t size,
                  bool& left);
  bool hashLocal(const char* path, uint32_t size,
                 uint8_t digest[Sha1::kDigestSize]);
  bool ensureDirectory(const char* relativeFile);
  bool vfs(VfsOperation operation, const char* path, uint32_t value,
           VfsResult& result, uint32_t timeoutMs);
  // Итог замены файла копией: kDone — копия стоит на имени файла;
  // kUntouched — ничего не менялось, копию можно удалить; kUnknown — исход
  // неизвестен (сбой носителя, обрыв связи, любой отказ RENAME): копию
  // удалять нельзя.
  enum class Replace : uint8_t { kDone, kUntouched, kUnknown };
  // keepExisting — защищённый файл: ставится, только если его на SD нет.
  // relativeFile — путь файла от корня WC: по нему отмечается остаток.
  Replace vfsReplace(const char* tempPath, const char* finalPath,
                     const char* finalName, const char* asidePath,
                     const char* relativeFile, bool keepExisting);
  Replace renameReplace(const char* tempPath, const char* finalPath,
                        const char* finalName, const char* asidePath,
                        const char* relativeFile, bool keepExisting);
  // RENAME в пределах каталога; false — отказ или обрыв связи.
  bool renameEntry(const char* oldPath, const char* newName);
  // Номер каталога эталона: первые length байтов dir без учёта регистра;
  // -1 — такого нет.
  int directoryIndex(const char* dir, size_t length) const;
  // Номер каталога эталона, где лежит файл relativeFile.
  int directoryOf(const char* relativeFile) const;
  bool leftoverIn(const char* relativeFile) const;
  void markLeftover(const char* relativeFile);
  bool probeLeftovers(size_t index, const char* directoryPath);
  // Запрос уборки: его отказ не подменяет причину неудачи в error_.
  bool quietVfs(VfsOperation operation, const char* path, uint32_t timeoutMs);
  bool awaitVfs(VfsResult& result, uint32_t timeoutMs);

  bool sendState(WcPhase phase, uint16_t current, uint16_t total,
                 const char* format, ...);
  // Ход этапа в байтах: полоса прогресса плагина. Событие уходит, когда
  // процент изменился и прошло не меньше kProgressIntervalMs.
  void beginProgress(uint32_t totalBytes);
  void addProgress(uint32_t bytes);
  bool sendEntry(size_t index);
  bool sendReady();
  void fail(const char* format, ...);

  bool isProtected(const char* path) const;
  WcFile* findFile(const char* path);
  static void localPath(const char* relative, char* output, size_t capacity);
  static bool urlEncodePath(const char* path, char* output, size_t capacity);

  VfsBridge& bridge_;
  WcFetcher& fetcher_;
  EventSink sink_;
  void* context_;
  volatile bool stop_;
  // Замена кончилась неизвестным исходом: карту надо проверить, дальше в
  // этом сеансе не писать.
  bool diskSuspect_;
  // Список SD не влез в таблицу: чужие файлы показаны не все.
  bool listTruncated_;
  // Опись нашла WCUPD.TMP или WCUPD.OLD: прерванная замена. Такой остаток
  // может делить цепочку кластеров с файлом (сбой с неподтверждённым
  // откатом), и удалять его нельзя; запись в его каталог запрещена до
  // проверки карты.
  bool leftoverSeen_;
  WcFile* files_;
  size_t fileCount_;
  char repo_[96];
  char branch_[64];
  char directory_[64];
  char protected_[kMaxProtected][GitTreeEntry::kPathSize];
  size_t protectedCount_;
  char commit_[41];
  char error_[96];
  // Каталоги с GitHub (включая корень "") — именно их опись и нужна.
  static constexpr size_t kMaxDirectories = 16;
  char directories_[kMaxDirectories][GitTreeEntry::kPathSize];
  bool leftover_[kMaxDirectories];
  // Опись прочитала каталог (он есть на SD) либо обновлятор создал его
  // сам. Каталог, который опись сочла отсутствующим, а запись нашла, —
  // признак ошибок чтения: остаток в нём мог остаться незамеченным.
  bool present_[kMaxDirectories];
  size_t directoryCount_;
  // Последнее состояние — чтобы повторить его с новым процентом.
  WcPhase statePhase_;
  uint16_t stateCurrent_;
  uint16_t stateTotal_;
  char stateText_[64];
  uint32_t progressTotal_;
  uint32_t progressDone_;
  uint8_t progressPercent_;
  uint32_t progressSentMs_;
  uint8_t chunk_[1024];
};

}  // namespace zifi
