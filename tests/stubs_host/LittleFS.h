#pragma once

// Кольцевой лог на хосте пишется в stdout, а не во flash: файловая система
// прошивки здесь не нужна, а терять записи нельзя — именно они показывают ход
// обмена так же, как на железе.

#include <Arduino.h>

class HostFile {
 public:
  operator bool() const { return false; }
  size_t write(const uint8_t*, size_t length) { return length; }
  size_t read(uint8_t*, size_t) { return 0; }
  void close() {}
  void flush() {}
  size_t size() const { return 0; }
  bool seek(size_t, int = 0) { return false; }
};

using File = HostFile;

class LittleFSClass {
 public:
  bool begin(bool = false, const char* = "", uint8_t = 10,
             const char* = "") { return false; }
  void end() {}
  File open(const char*, const char* = "r") { return File(); }
  bool exists(const char*) { return false; }
  bool remove(const char*) { return false; }
  bool rename(const char*, const char*) { return false; }
};

extern LittleFSClass LittleFS;
