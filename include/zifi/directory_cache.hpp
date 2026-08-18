#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Кэш хранит полные снимки каталогов в PSRAM, пока работает SMB-плагин.
// В ZiFi карта памяти в это время принадлежит только SMB-серверу, поэтому
// таймер устаревания не нужен: сервер сам знает обо всех изменениях файлов.
class DirectoryCache {
 public:
  // Из 2 МиБ PSRAM заранее зарезервированы потоковые кольца и очередь
  // параллельных SMB WRITE. Половины мегабайта хватает на многие тысячи
  // обычных FAT-записей и оставляет гарантированный запас сетевому конвейеру.
  static constexpr size_t kMaximumBytes = 512 * 1024;

  struct EntryView {
    bool isDirectory = false;
    uint32_t size = 0;
    const char* name = nullptr;
  };

  // Cursor содержит только непрозрачный указатель. После любой инвалидации
  // revision меняется, и старый указатель не разыменовывается.
  struct Cursor {
    const void* next = nullptr;
    uint32_t revision = 0;
    bool opened = false;
  };

  DirectoryCache();
  ~DirectoryCache();

  DirectoryCache(const DirectoryCache&) = delete;
  DirectoryCache& operator=(const DirectoryCache&) = delete;

  // Без PSRAM сервер продолжит работать прежним потоковым способом.
  bool begin(bool psramAvailable);
  void clear();
  bool enabled() const { return enabled_; }
  size_t bytesUsed() const { return bytesUsed_; }

  bool contains(const char* path) const;
  bool beginSnapshot(const char* path);
  bool append(bool isDirectory, uint32_t size, const char* name);
  bool finishSnapshot();
  void abortSnapshot();

  // Каталог, который не поместился в кэш целиком, помечается заглушкой. Без
  // неё сервер повторял полный обход по UART при каждом обращении и каждый раз
  // упирался в тот же предел. Заглушка снимается обычной инвалидацией, поэтому
  // после удаления лишних файлов каталог снова становится кэшируемым.
  bool markUncacheable(const char* path);
  bool isUncacheable(const char* path) const;

  // Точечное обновление размера уже закэшированной записи. Применяется во
  // время записи файла: снимок остаётся валидным, revision не меняется, и
  // открытые курсоры Проводника не сбрасываются на каждом блоке.
  bool updateEntrySize(const char* path, const char* name, uint32_t size);
  // То же самое, но путь родителя задаётся длиной внутри исходной строки.
  // Вызывается на каждом окне записи, поэтому копий пути на стеке не делает:
  // стек SMB-задачи ограничен, а два буфера по 257 байт там уже опасны.
  bool updateEntrySizeAt(const char* path, size_t parentLength,
                         const char* name, uint32_t size);

  // invalidate удаляет один каталог, invalidateSubtree — каталог и все
  // вложенные снимки. Второй вариант нужен при удалении/переименовании папки.
  void invalidate(const char* path);
  void invalidateSubtree(const char* path);

  bool openCursor(const char* path, Cursor& cursor) const;
  bool cursorCurrent(const Cursor& cursor) const;
  bool next(Cursor& cursor, EntryView& entry) const;
  bool findEntry(const char* path, const char* name, EntryView& entry) const;

 private:
  struct Entry;
  struct Snapshot;

  void* allocate(size_t bytes);
  void release(void* memory, size_t bytes);
  void freeSnapshot(Snapshot* snapshot);
  Snapshot* findSnapshot(const char* path) const;
  Snapshot* findAnySnapshot(const char* path) const;
  // Освобождает снимок, к которому дольше всего не обращались. Строящийся
  // снимок не вытесняется никогда: он ещё не является цельными данными.
  bool evictOldest();
  void advanceRevision();
  static bool equalNoCase(const char* left, const char* right);
  static bool belongsToSubtree(const char* value, const char* root);

  Snapshot* snapshots_;
  Snapshot* building_;
  size_t bytesUsed_;
  uint32_t revision_;
  // Счётчик обращений играет роль часов LRU: реальное время здесь не нужно и
  // не должно зависеть от millis() ради тестируемости на ПК.
  mutable uint32_t useCounter_;
  bool enabled_;
};

}  // namespace zifi
