#pragma once

#include <stddef.h>
#include <stdint.h>

namespace zifi {

// Компактная копия состояния выбранной активной FAT в PSRAM. Один бит хранит
// занятость одного data-кластера; для текущей 32-ГБ карты это 122064 байта
// вместо примерно 3,9 МБ сырой FAT.
class FatAllocationCache {
 public:
  static constexpr size_t kMaximumBytes = 512 * 1024;

  FatAllocationCache();
  ~FatAllocationCache();

  FatAllocationCache(const FatAllocationCache&) = delete;
  FatAllocationCache& operator=(const FatAllocationCache&) = delete;

  bool begin(bool psramAvailable);
  void clear();
  void invalidate();

  bool prepare(uint32_t totalClusters, uint32_t fatSectors, uint32_t serial,
               uint8_t activeFat);
  // Каждый входной бит соответствует одной четырёхбайтовой FAT32-записи:
  // 1 — занята, 0 — свободна. rawOffset относится к исходной сырой FAT.
  bool ingestMap(uint32_t rawOffset, const uint8_t* data, size_t length);
  bool finish();

  bool matches(uint32_t totalClusters, uint32_t fatSectors, uint32_t serial,
               uint8_t activeFat) const;
  bool valid() const { return valid_; }
  uint32_t freeClusters() const { return freeClusters_; }
  size_t bytesUsed() const { return bytes_; }
  bool allocated(uint32_t cluster) const;

  // Этот кэш — единственный владелец числа свободных кластеров. Изменение
  // длины файла меняет его предсказуемо, поэтому счётчик правится здесь, а не
  // выбрасывается вместе со всей картой: пересканирование активной FAT на
  // каждом окне записи и на каждом удалённом файле было дороже самой операции.
  //
  // Битовая карта после такой правки описывает занятость приблизительно, ведь
  // номера выделенных Wild Commander кластеров сюда не сообщаются. Точным
  // остаётся именно счётчик — а он и есть то, что спрашивает SMB.
  void noteResize(uint32_t oldSize, uint32_t newSize, uint32_t bytesPerCluster);
  bool exactMap() const { return exactMap_; }

 private:
  void* allocate(size_t bytes);
  void release();

  uint8_t* bits_;
  size_t bytes_;
  uint32_t totalClusters_;
  uint32_t fatSectors_;
  uint32_t serial_;
  uint32_t rawOffset_;
  uint32_t freeClusters_;
  uint8_t activeFat_;
  bool enabled_;
  bool building_;
  bool valid_;
  // false — счётчик правился арифметикой и биты карты уже не описывают
  // конкретные кластеры. Для allocated() это значит «ответ недостоверен».
  bool exactMap_;
};

}  // namespace zifi
