#include "zifi/directory_cache.hpp"

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

namespace zifi {

// Имя переменной длины экономит PSRAM: короткое FAT-имя не занимает
// фиксированные 256 байт. Каждый элемент освобождается вместе со снимком.
struct DirectoryCache::Entry {
  Entry* next;
  uint32_t size;
  bool isDirectory;
  char name[1];
};

struct DirectoryCache::Snapshot {
  Snapshot* next;
  Entry* first;
  Entry* last;
  uint32_t count;
  uint32_t lastUse;
  bool complete;
  // Заглушка «каталог не помещается в кэш»: сам список записей пуст, снимок
  // нужен только чтобы не повторять заведомо провальный обход по UART.
  bool uncacheable;
  char path[1];
};

DirectoryCache::DirectoryCache()
    : snapshots_(nullptr),
      building_(nullptr),
      bytesUsed_(0),
      revision_(1),
      useCounter_(1),
      enabled_(false) {}

DirectoryCache::~DirectoryCache() { clear(); }

bool DirectoryCache::begin(bool psramAvailable) {
  clear();
  enabled_ = psramAvailable;
  return enabled_;
}

void* DirectoryCache::allocate(size_t bytes) {
  if (!enabled_ || bytes == 0 || bytes > kMaximumBytes) {
    return nullptr;
  }
  // Пока не хватает бюджета, отдаём место самого давнего снимка. Без этого
  // первый же большой каталог занимал кэш навсегда: новые снимки не строились,
  // а старые никто не освобождал до перезапуска плагина.
  while (bytesUsed_ > kMaximumBytes || bytes > kMaximumBytes - bytesUsed_) {
    if (!evictOldest()) {
      return nullptr;
    }
  }
#if defined(ESP_PLATFORM)
  void* memory =
      heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  // В обычной прошивке эта ветка не используется. Она позволяет проверить
  // всю логику кэша быстрым тестом на ПК без эмуляции ESP32.
  void* memory = malloc(bytes);
#endif
  if (memory != nullptr) {
    bytesUsed_ += bytes;
  }
  return memory;
}

void DirectoryCache::release(void* memory, size_t bytes) {
  if (memory == nullptr) {
    return;
  }
#if defined(ESP_PLATFORM)
  heap_caps_free(memory);
#else
  free(memory);
#endif
  bytesUsed_ = bytes <= bytesUsed_ ? bytesUsed_ - bytes : 0;
}

bool DirectoryCache::equalNoCase(const char* left, const char* right) {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  while (*left != 0 && *right != 0) {
    const unsigned char a = static_cast<unsigned char>(*left++);
    const unsigned char b = static_cast<unsigned char>(*right++);
    // ASCII складываем без учёта регистра, а UTF-8 байты сравниваем как есть.
    if ((a < 0x80 ? toupper(a) : a) != (b < 0x80 ? toupper(b) : b)) {
      return false;
    }
  }
  return *left == 0 && *right == 0;
}

bool DirectoryCache::belongsToSubtree(const char* value, const char* root) {
  if (value == nullptr || root == nullptr) {
    return false;
  }
  if (strcmp(root, "/") == 0) {
    return value[0] == '/';
  }
  size_t index = 0;
  while (root[index] != 0 && value[index] != 0) {
    const unsigned char a = static_cast<unsigned char>(value[index]);
    const unsigned char b = static_cast<unsigned char>(root[index]);
    if ((a < 0x80 ? toupper(a) : a) != (b < 0x80 ? toupper(b) : b)) {
      return false;
    }
    ++index;
  }
  return root[index] == 0 &&
         (value[index] == 0 || value[index] == '/');
}

DirectoryCache::Snapshot* DirectoryCache::findAnySnapshot(
    const char* path) const {
  for (Snapshot* snapshot = snapshots_; snapshot != nullptr;
       snapshot = snapshot->next) {
    if (snapshot->complete && equalNoCase(snapshot->path, path)) {
      return snapshot;
    }
  }
  return nullptr;
}

DirectoryCache::Snapshot* DirectoryCache::findSnapshot(
    const char* path) const {
  Snapshot* snapshot = findAnySnapshot(path);
  if (snapshot == nullptr || snapshot->uncacheable) {
    return nullptr;
  }
  // Каждое попадание освежает метку LRU, поэтому вытесняется именно тот
  // каталог, в который дольше всего никто не заходил.
  snapshot->lastUse = ++useCounter_;
  return snapshot;
}

bool DirectoryCache::evictOldest() {
  Snapshot** victimLink = nullptr;
  uint32_t oldest = 0;
  for (Snapshot** link = &snapshots_; *link != nullptr;
       link = &(*link)->next) {
    Snapshot* snapshot = *link;
    if (snapshot == building_) {
      continue;
    }
    if (victimLink == nullptr || snapshot->lastUse < oldest) {
      victimLink = link;
      oldest = snapshot->lastUse;
    }
  }
  if (victimLink == nullptr) {
    return false;
  }
  Snapshot* victim = *victimLink;
  *victimLink = victim->next;
  freeSnapshot(victim);
  // Освобождение меняет набор снимков, поэтому чужие курсоры обязаны
  // перечитать своё положение перед следующим разыменованием.
  advanceRevision();
  return true;
}

bool DirectoryCache::contains(const char* path) const {
  return enabled_ && findSnapshot(path) != nullptr;
}

bool DirectoryCache::isUncacheable(const char* path) const {
  if (!enabled_) {
    return false;
  }
  Snapshot* snapshot = findAnySnapshot(path);
  return snapshot != nullptr && snapshot->uncacheable;
}

bool DirectoryCache::markUncacheable(const char* path) {
  if (!enabled_ || path == nullptr || path[0] != '/' || building_ != nullptr) {
    return false;
  }
  invalidate(path);
  const size_t bytes = offsetof(Snapshot, path) + strlen(path) + 1;
  auto* snapshot = static_cast<Snapshot*>(allocate(bytes));
  if (snapshot == nullptr) {
    return false;
  }
  memset(snapshot, 0, bytes);
  memcpy(snapshot->path, path, strlen(path) + 1);
  snapshot->complete = true;
  snapshot->uncacheable = true;
  snapshot->lastUse = ++useCounter_;
  snapshot->next = snapshots_;
  snapshots_ = snapshot;
  return true;
}

bool DirectoryCache::updateEntrySizeAt(const char* path, size_t parentLength,
                                       const char* name, uint32_t size) {
  if (!enabled_ || path == nullptr || name == nullptr || parentLength == 0) {
    return false;
  }
  // Сравниваем родителя прямо в исходной строке, не копируя её.
  Snapshot* snapshot = nullptr;
  for (Snapshot* candidate = snapshots_; candidate != nullptr;
       candidate = candidate->next) {
    if (!candidate->complete || candidate->uncacheable) {
      continue;
    }
    size_t index = 0;
    while (index < parentLength && candidate->path[index] != 0) {
      const unsigned char a = static_cast<unsigned char>(candidate->path[index]);
      const unsigned char b = static_cast<unsigned char>(path[index]);
      if ((a < 0x80 ? toupper(a) : a) != (b < 0x80 ? toupper(b) : b)) {
        break;
      }
      ++index;
    }
    if (index == parentLength && candidate->path[parentLength] == 0) {
      snapshot = candidate;
      break;
    }
  }
  if (snapshot == nullptr) {
    return false;
  }
  snapshot->lastUse = ++useCounter_;
  for (Entry* entry = snapshot->first; entry != nullptr; entry = entry->next) {
    if (!entry->isDirectory && equalNoCase(entry->name, name)) {
      entry->size = size;
      return true;
    }
  }
  return false;
}

bool DirectoryCache::updateEntrySize(const char* path, const char* name,
                                     uint32_t size) {
  Snapshot* snapshot = enabled_ ? findSnapshot(path) : nullptr;
  if (snapshot == nullptr || name == nullptr) {
    return false;
  }
  for (Entry* entry = snapshot->first; entry != nullptr; entry = entry->next) {
    if (!entry->isDirectory && equalNoCase(entry->name, name)) {
      // Меняется только значение внутри уже существующей записи, поэтому
      // revision оставляем прежним: список не перестраивался, а сбрасывать
      // курсоры Проводника на каждом блоке записи слишком дорого.
      entry->size = size;
      return true;
    }
  }
  return false;
}

void DirectoryCache::advanceRevision() {
  ++revision_;
  if (revision_ == 0) {
    revision_ = 1;
  }
}

void DirectoryCache::freeSnapshot(Snapshot* snapshot) {
  if (snapshot == nullptr) {
    return;
  }
  Entry* entry = snapshot->first;
  while (entry != nullptr) {
    Entry* next = entry->next;
    const size_t bytes = offsetof(Entry, name) + strlen(entry->name) + 1;
    release(entry, bytes);
    entry = next;
  }
  const size_t bytes = offsetof(Snapshot, path) + strlen(snapshot->path) + 1;
  release(snapshot, bytes);
}

void DirectoryCache::clear() {
  Snapshot* snapshot = snapshots_;
  while (snapshot != nullptr) {
    Snapshot* next = snapshot->next;
    freeSnapshot(snapshot);
    snapshot = next;
  }
  snapshots_ = nullptr;
  building_ = nullptr;
  bytesUsed_ = 0;
  enabled_ = false;
  advanceRevision();
}

bool DirectoryCache::beginSnapshot(const char* path) {
  if (!enabled_ || path == nullptr || path[0] != '/' || building_ != nullptr) {
    return false;
  }
  invalidate(path);
  const size_t bytes = offsetof(Snapshot, path) + strlen(path) + 1;
  auto* snapshot = static_cast<Snapshot*>(allocate(bytes));
  if (snapshot == nullptr) {
    return false;
  }
  memset(snapshot, 0, bytes);
  memcpy(snapshot->path, path, strlen(path) + 1);
  snapshot->lastUse = ++useCounter_;
  snapshot->next = snapshots_;
  snapshots_ = snapshot;
  building_ = snapshot;
  return true;
}

bool DirectoryCache::append(bool isDirectory, uint32_t size,
                            const char* name) {
  if (building_ == nullptr || name == nullptr || name[0] == 0 ||
      strlen(name) > 255) {
    return false;
  }
  const size_t bytes = offsetof(Entry, name) + strlen(name) + 1;
  auto* entry = static_cast<Entry*>(allocate(bytes));
  if (entry == nullptr) {
    return false;
  }
  memset(entry, 0, bytes);
  entry->size = size;
  entry->isDirectory = isDirectory;
  memcpy(entry->name, name, strlen(name) + 1);
  if (building_->last == nullptr) {
    building_->first = entry;
  } else {
    building_->last->next = entry;
  }
  building_->last = entry;
  ++building_->count;
  return true;
}

bool DirectoryCache::finishSnapshot() {
  if (building_ == nullptr) {
    return false;
  }
  building_->complete = true;
  building_ = nullptr;
  advanceRevision();
  return true;
}

void DirectoryCache::abortSnapshot() {
  if (building_ == nullptr) {
    return;
  }
  Snapshot** link = &snapshots_;
  while (*link != nullptr && *link != building_) {
    link = &(*link)->next;
  }
  Snapshot* discarded = building_;
  if (*link == discarded) {
    *link = discarded->next;
  }
  building_ = nullptr;
  freeSnapshot(discarded);
}

void DirectoryCache::invalidate(const char* path) {
  if (path == nullptr) {
    return;
  }
  bool changed = false;
  Snapshot** link = &snapshots_;
  while (*link != nullptr) {
    Snapshot* snapshot = *link;
    if (snapshot != building_ && equalNoCase(snapshot->path, path)) {
      *link = snapshot->next;
      freeSnapshot(snapshot);
      changed = true;
    } else {
      link = &snapshot->next;
    }
  }
  if (changed) {
    advanceRevision();
  }
}

void DirectoryCache::invalidateSubtree(const char* path) {
  if (path == nullptr) {
    return;
  }
  bool changed = false;
  Snapshot** link = &snapshots_;
  while (*link != nullptr) {
    Snapshot* snapshot = *link;
    if (snapshot != building_ && belongsToSubtree(snapshot->path, path)) {
      *link = snapshot->next;
      freeSnapshot(snapshot);
      changed = true;
    } else {
      link = &snapshot->next;
    }
  }
  if (changed) {
    advanceRevision();
  }
}

bool DirectoryCache::openCursor(const char* path, Cursor& cursor) const {
  Snapshot* snapshot = enabled_ ? findSnapshot(path) : nullptr;
  if (snapshot == nullptr) {
    cursor = {};
    return false;
  }
  cursor.next = snapshot->first;
  cursor.revision = revision_;
  cursor.opened = true;
  return true;
}

bool DirectoryCache::cursorCurrent(const Cursor& cursor) const {
  return cursor.opened && cursor.revision == revision_;
}

bool DirectoryCache::next(Cursor& cursor, EntryView& view) const {
  if (!cursorCurrent(cursor) || cursor.next == nullptr) {
    return false;
  }
  const auto* entry = static_cast<const Entry*>(cursor.next);
  view.isDirectory = entry->isDirectory;
  view.size = entry->size;
  view.name = entry->name;
  cursor.next = entry->next;
  return true;
}

bool DirectoryCache::findEntry(const char* path, const char* name,
                               EntryView& view) const {
  Snapshot* snapshot = enabled_ ? findSnapshot(path) : nullptr;
  if (snapshot == nullptr || name == nullptr) {
    return false;
  }
  for (Entry* entry = snapshot->first; entry != nullptr; entry = entry->next) {
    if (equalNoCase(entry->name, name)) {
      view.isDirectory = entry->isDirectory;
      view.size = entry->size;
      view.name = entry->name;
      return true;
    }
  }
  return false;
}

}  // namespace zifi
