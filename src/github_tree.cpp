#include "zifi/github_tree.hpp"

#include <string.h>

namespace zifi {
namespace {

// Минимальный разбор JSON ровно в том объёме, что нужен ответам GitHub:
// объекты, массивы, строки с экранированием, целые числа и литералы.
// Любое нарушение синтаксиса — отказ: эталон с GitHub не угадывается.
class JsonCursor {
 public:
  JsonCursor(const char* text, size_t length)
      : position_(text), end_(text + length) {}

  void skipSpace() {
    while (position_ < end_ && (*position_ == ' ' || *position_ == '\t' ||
                                *position_ == '\r' || *position_ == '\n')) {
      ++position_;
    }
  }

  bool peek(char expected) {
    skipSpace();
    return position_ < end_ && *position_ == expected;
  }

  bool take(char expected) {
    if (!peek(expected)) {
      return false;
    }
    ++position_;
    return true;
  }

  // Строка в output (UTF-8, с нулём). output == nullptr — только пропустить.
  // Не поместилась — отказ.
  bool string(char* output, size_t capacity) {
    if (!take('"')) {
      return false;
    }
    size_t used = 0;
    while (position_ < end_) {
      char symbol = *position_++;
      if (symbol == '"') {
        if (output != nullptr) {
          output[used] = 0;
        }
        return true;
      }
      if (static_cast<unsigned char>(symbol) < 0x20) {
        return false;
      }
      if (symbol != '\\') {
        if (!put(output, capacity, used, symbol)) {
          return false;
        }
        continue;
      }
      if (position_ >= end_) {
        return false;
      }
      symbol = *position_++;
      switch (symbol) {
        case '"':
        case '\\':
        case '/':
          break;
        case 'b':
          symbol = '\b';
          break;
        case 'f':
          symbol = '\f';
          break;
        case 'n':
          symbol = '\n';
          break;
        case 'r':
          symbol = '\r';
          break;
        case 't':
          symbol = '\t';
          break;
        case 'u': {
          uint32_t code = 0;
          // \u0000 внутри строки дал бы C-строку короче значения: путь
          // сравнивался бы по префиксу. Такой ответ — не эталон.
          if (!hex4(code) || code == 0) {
            return false;
          }
          if (code >= 0xD800 && code <= 0xDBFF) {
            uint32_t low = 0;
            if (end_ - position_ < 2 || position_[0] != '\\' ||
                position_[1] != 'u') {
              return false;
            }
            position_ += 2;
            if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) {
              return false;
            }
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          } else if (code >= 0xDC00 && code <= 0xDFFF) {
            return false;
          }
          if (!putUtf8(output, capacity, used, code)) {
            return false;
          }
          continue;
        }
        default:
          return false;
      }
      if (!put(output, capacity, used, symbol)) {
        return false;
      }
    }
    return false;
  }

  bool number(uint64_t& value) {
    skipSpace();
    if (position_ >= end_ || *position_ < '0' || *position_ > '9') {
      return false;
    }
    // Ведущий ноль JSON разрешает только у самого числа 0.
    if (*position_ == '0' && end_ - position_ > 1 && position_[1] >= '0' &&
        position_[1] <= '9') {
      return false;
    }
    value = 0;
    while (position_ < end_ && *position_ >= '0' && *position_ <= '9') {
      const uint64_t next = value * 10U + static_cast<unsigned>(*position_ - '0');
      if (next < value) {
        return false;
      }
      value = next;
      ++position_;
    }
    // Дробь или порядок в полях, которые нам нужны, не встречаются.
    return position_ >= end_ ||
           (*position_ != '.' && *position_ != 'e' && *position_ != 'E');
  }

  bool literal(const char* word) {
    skipSpace();
    const size_t length = strlen(word);
    if (static_cast<size_t>(end_ - position_) < length ||
        memcmp(position_, word, length) != 0) {
      return false;
    }
    position_ += length;
    return true;
  }

  // Пропустить любое значение, включая вложенные объекты и массивы.
  bool skipValue(unsigned depth = 0) {
    if (depth > 32) {
      return false;
    }
    skipSpace();
    if (position_ >= end_) {
      return false;
    }
    switch (*position_) {
      case '"':
        return string(nullptr, 0);
      case '{':
        ++position_;
        if (take('}')) {
          return true;
        }
        do {
          if (!string(nullptr, 0) || !take(':') || !skipValue(depth + 1)) {
            return false;
          }
        } while (take(','));
        return take('}');
      case '[':
        ++position_;
        if (take(']')) {
          return true;
        }
        do {
          if (!skipValue(depth + 1)) {
            return false;
          }
        } while (take(','));
        return take(']');
      case 't':
        return literal("true");
      case 'f':
        return literal("false");
      case 'n':
        return literal("null");
      default:
        return skipNumber();
    }
  }

  bool atEnd() {
    skipSpace();
    return position_ >= end_;
  }

  // Число по грамматике JSON: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
  bool skipNumber() {
    if (position_ < end_ && *position_ == '-') {
      ++position_;
    }
    if (position_ >= end_ || !digit(*position_)) {
      return false;
    }
    if (*position_ == '0') {
      ++position_;
    } else {
      skipDigits();
    }
    if (position_ < end_ && *position_ == '.') {
      ++position_;
      if (position_ >= end_ || !digit(*position_)) {
        return false;
      }
      skipDigits();
    }
    if (position_ < end_ && (*position_ == 'e' || *position_ == 'E')) {
      ++position_;
      if (position_ < end_ && (*position_ == '+' || *position_ == '-')) {
        ++position_;
      }
      if (position_ >= end_ || !digit(*position_)) {
        return false;
      }
      skipDigits();
    }
    return true;
  }

 private:
  static bool digit(char symbol) { return symbol >= '0' && symbol <= '9'; }

  void skipDigits() {
    while (position_ < end_ && digit(*position_)) {
      ++position_;
    }
  }

  bool hex4(uint32_t& value) {
    if (end_ - position_ < 4) {
      return false;
    }
    value = 0;
    for (int index = 0; index < 4; ++index) {
      const char digit = *position_++;
      value <<= 4;
      if (digit >= '0' && digit <= '9') {
        value |= static_cast<uint32_t>(digit - '0');
      } else if (digit >= 'a' && digit <= 'f') {
        value |= static_cast<uint32_t>(digit - 'a' + 10);
      } else if (digit >= 'A' && digit <= 'F') {
        value |= static_cast<uint32_t>(digit - 'A' + 10);
      } else {
        return false;
      }
    }
    return true;
  }

  static bool put(char* output, size_t capacity, size_t& used, char symbol) {
    if (output == nullptr) {
      return true;
    }
    if (used + 1 >= capacity) {
      return false;
    }
    output[used++] = symbol;
    return true;
  }

  static bool putUtf8(char* output, size_t capacity, size_t& used,
                      uint32_t code) {
    if (code < 0x80) {
      return put(output, capacity, used, static_cast<char>(code));
    }
    if (code < 0x800) {
      return put(output, capacity, used, static_cast<char>(0xC0 | code >> 6)) &&
             put(output, capacity, used,
                 static_cast<char>(0x80 | (code & 0x3F)));
    }
    if (code < 0x10000) {
      return put(output, capacity, used,
                 static_cast<char>(0xE0 | code >> 12)) &&
             put(output, capacity, used,
                 static_cast<char>(0x80 | (code >> 6 & 0x3F))) &&
             put(output, capacity, used,
                 static_cast<char>(0x80 | (code & 0x3F)));
    }
    return put(output, capacity, used, static_cast<char>(0xF0 | code >> 18)) &&
           put(output, capacity, used,
               static_cast<char>(0x80 | (code >> 12 & 0x3F))) &&
           put(output, capacity, used,
               static_cast<char>(0x80 | (code >> 6 & 0x3F))) &&
           put(output, capacity, used, static_cast<char>(0x80 | (code & 0x3F)));
  }

  const char* position_;
  const char* end_;
};

// Прочитать ключ объекта. Ключи ответов GitHub короткие; длинный — чужой,
// его значение всё равно пропускается.
bool readKey(JsonCursor& json, char* key, size_t capacity) {
  return json.string(key, capacity) && json.take(':');
}

bool isHex40(const char* text) {
  if (strlen(text) != 40) {
    return false;
  }
  uint8_t digest[Sha1::kDigestSize];
  return parseSha1Hex(text, digest);
}

// Путь git: непустой, без ведущего '/', без пустых частей и без «.»/«..».
bool pathIsSafe(const char* path) {
  if (path[0] == 0 || path[0] == '/') {
    return false;
  }
  const char* part = path;
  for (;;) {
    const char* slash = strchr(part, '/');
    const size_t length = slash == nullptr ? strlen(part)
                                           : static_cast<size_t>(slash - part);
    if (length == 0 || (length == 1 && part[0] == '.') ||
        (length == 2 && part[0] == '.' && part[1] == '.')) {
      return false;
    }
    for (size_t index = 0; index < length; ++index) {
      const unsigned char symbol = static_cast<unsigned char>(part[index]);
      if (symbol < 0x20 || symbol == '\\') {
        return false;
      }
    }
    if (slash == nullptr) {
      return true;
    }
    part = slash + 1;
  }
}

}  // namespace

bool parseGitRefCommit(const char* json, size_t length, char commit[41]) {
  if (json == nullptr || commit == nullptr) {
    return false;
  }
  JsonCursor cursor(json, length);
  if (!cursor.take('{')) {
    return false;
  }
  bool found = false;
  char key[16];
  if (!cursor.peek('}')) {
    do {
      if (!readKey(cursor, key, sizeof(key))) {
        return false;
      }
      if (strcmp(key, "object") != 0) {
        if (!cursor.skipValue()) {
          return false;
        }
        continue;
      }
      if (!cursor.take('{')) {
        return false;
      }
      char type[16] = {};
      char sha[48] = {};
      if (!cursor.peek('}')) {
        do {
          if (!readKey(cursor, key, sizeof(key))) {
            return false;
          }
          if (strcmp(key, "sha") == 0) {
            if (!cursor.string(sha, sizeof(sha))) {
              return false;
            }
          } else if (strcmp(key, "type") == 0) {
            if (!cursor.string(type, sizeof(type))) {
              return false;
            }
          } else if (!cursor.skipValue()) {
            return false;
          }
        } while (cursor.take(','));
      }
      if (!cursor.take('}') || strcmp(type, "commit") != 0 || !isHex40(sha)) {
        return false;
      }
      memcpy(commit, sha, 41);
      found = true;
    } while (cursor.take(','));
  }
  return cursor.take('}') && cursor.atEnd() && found;
}

bool parseGitTree(const char* json, size_t length, GitTreeEntry* entries,
                  size_t capacity, size_t& count, bool& truncated) {
  count = 0;
  truncated = true;
  if (json == nullptr || entries == nullptr) {
    return false;
  }
  JsonCursor cursor(json, length);
  if (!cursor.take('{')) {
    return false;
  }
  bool haveTree = false;
  bool haveTruncated = false;
  char key[16];
  if (!cursor.peek('}')) {
    do {
      if (!readKey(cursor, key, sizeof(key))) {
        return false;
      }
      if (strcmp(key, "truncated") == 0) {
        if (cursor.literal("true")) {
          truncated = true;
        } else if (cursor.literal("false")) {
          truncated = false;
        } else {
          return false;
        }
        haveTruncated = true;
        continue;
      }
      if (strcmp(key, "tree") != 0) {
        if (!cursor.skipValue()) {
          return false;
        }
        continue;
      }
      if (!cursor.take('[')) {
        return false;
      }
      haveTree = true;
      if (cursor.take(']')) {
        continue;
      }
      do {
        if (!cursor.take('{')) {
          return false;
        }
        GitTreeEntry entry;
        char type[16] = {};
        char sha[48] = {};
        bool haveSize = false;
        bool havePath = false;
        if (!cursor.peek('}')) {
          do {
            if (!readKey(cursor, key, sizeof(key))) {
              return false;
            }
            if (strcmp(key, "path") == 0) {
              if (!cursor.string(entry.path, sizeof(entry.path))) {
                return false;
              }
              havePath = true;
            } else if (strcmp(key, "type") == 0) {
              if (!cursor.string(type, sizeof(type))) {
                return false;
              }
            } else if (strcmp(key, "sha") == 0) {
              if (!cursor.string(sha, sizeof(sha))) {
                return false;
              }
            } else if (strcmp(key, "size") == 0) {
              uint64_t size = 0;
              if (!cursor.number(size) || size > 0xFFFFFFFFULL) {
                return false;
              }
              entry.size = static_cast<uint32_t>(size);
              haveSize = true;
            } else if (!cursor.skipValue()) {
              return false;
            }
          } while (cursor.take(','));
        }
        if (!cursor.take('}')) {
          return false;
        }
        if (strcmp(type, "commit") == 0) {
          continue;  // подмодуль: у него нет содержимого в этом дереве
        }
        const bool blob = strcmp(type, "blob") == 0;
        if ((!blob && strcmp(type, "tree") != 0) || !havePath ||
            !pathIsSafe(entry.path) || !isHex40(sha) ||
            (blob && !haveSize)) {
          return false;
        }
        parseSha1Hex(sha, entry.sha);
        entry.directory = !blob;
        if (!blob) {
          entry.size = 0;
        }
        if (count >= capacity) {
          return false;
        }
        entries[count++] = entry;
      } while (cursor.take(','));
      if (!cursor.take(']')) {
        return false;
      }
    } while (cursor.take(','));
  }
  return cursor.take('}') && cursor.atEnd() && haveTree && haveTruncated;
}

}  // namespace zifi
