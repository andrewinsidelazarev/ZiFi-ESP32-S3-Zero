#include "zifi/git_sha1.hpp"

#include <stdio.h>
#include <string.h>

namespace zifi {
namespace {

uint32_t rotateLeft(uint32_t value, unsigned bits) {
  return (value << bits) | (value >> (32 - bits));
}

int hexValue(char digit) {
  if (digit >= '0' && digit <= '9') {
    return digit - '0';
  }
  if (digit >= 'a' && digit <= 'f') {
    return digit - 'a' + 10;
  }
  if (digit >= 'A' && digit <= 'F') {
    return digit - 'A' + 10;
  }
  return -1;
}

}  // namespace

void Sha1::reset() {
  state_[0] = 0x67452301UL;
  state_[1] = 0xEFCDAB89UL;
  state_[2] = 0x98BADCFEUL;
  state_[3] = 0x10325476UL;
  state_[4] = 0xC3D2E1F0UL;
  length_ = 0;
  used_ = 0;
}

void Sha1::block(const uint8_t* data) {
  uint32_t words[80];
  for (unsigned index = 0; index < 16; ++index) {
    words[index] = static_cast<uint32_t>(data[4 * index]) << 24 |
                   static_cast<uint32_t>(data[4 * index + 1]) << 16 |
                   static_cast<uint32_t>(data[4 * index + 2]) << 8 |
                   static_cast<uint32_t>(data[4 * index + 3]);
  }
  for (unsigned index = 16; index < 80; ++index) {
    words[index] = rotateLeft(words[index - 3] ^ words[index - 8] ^
                                  words[index - 14] ^ words[index - 16],
                              1);
  }
  uint32_t a = state_[0];
  uint32_t b = state_[1];
  uint32_t c = state_[2];
  uint32_t d = state_[3];
  uint32_t e = state_[4];
  for (unsigned index = 0; index < 80; ++index) {
    uint32_t f;
    uint32_t k;
    if (index < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999UL;
    } else if (index < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1UL;
    } else if (index < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCUL;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6UL;
    }
    const uint32_t next = rotateLeft(a, 5) + f + e + k + words[index];
    e = d;
    d = c;
    c = rotateLeft(b, 30);
    b = a;
    a = next;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
}

void Sha1::update(const uint8_t* data, size_t length) {
  if (data == nullptr) {
    return;
  }
  length_ += length;
  while (length != 0) {
    const size_t part = length < 64 - used_ ? length : 64 - used_;
    memcpy(buffer_ + used_, data, part);
    used_ += part;
    data += part;
    length -= part;
    if (used_ == 64) {
      block(buffer_);
      used_ = 0;
    }
  }
}

void Sha1::finish(uint8_t digest[kDigestSize]) {
  const uint64_t bits = length_ * 8U;
  const uint8_t marker = 0x80;
  update(&marker, 1);
  const uint8_t zero = 0;
  while (used_ != 56) {
    update(&zero, 1);
  }
  uint8_t tail[8];
  for (unsigned index = 0; index < 8; ++index) {
    tail[index] = static_cast<uint8_t>(bits >> (56 - 8 * index));
  }
  update(tail, sizeof(tail));
  for (unsigned index = 0; index < 5; ++index) {
    digest[4 * index] = static_cast<uint8_t>(state_[index] >> 24);
    digest[4 * index + 1] = static_cast<uint8_t>(state_[index] >> 16);
    digest[4 * index + 2] = static_cast<uint8_t>(state_[index] >> 8);
    digest[4 * index + 3] = static_cast<uint8_t>(state_[index]);
  }
  reset();
}

void gitBlobBegin(Sha1& sha, uint32_t size) {
  char header[24];
  const int length = snprintf(header, sizeof(header), "blob %lu",
                              static_cast<unsigned long>(size));
  sha.reset();
  // Завершающий ноль заголовка — часть объекта git.
  sha.update(reinterpret_cast<const uint8_t*>(header),
             static_cast<size_t>(length) + 1);
}

bool parseSha1Hex(const char* text, uint8_t digest[Sha1::kDigestSize]) {
  if (text == nullptr) {
    return false;
  }
  for (size_t index = 0; index < Sha1::kDigestSize; ++index) {
    const int high = hexValue(text[2 * index]);
    const int low = high < 0 ? -1 : hexValue(text[2 * index + 1]);
    if (low < 0) {
      return false;
    }
    digest[index] = static_cast<uint8_t>(high << 4 | low);
  }
  return true;
}

void formatSha1Hex(const uint8_t digest[Sha1::kDigestSize], char text[41]) {
  static const char kDigits[] = "0123456789abcdef";
  for (size_t index = 0; index < Sha1::kDigestSize; ++index) {
    text[2 * index] = kDigits[digest[index] >> 4];
    text[2 * index + 1] = kDigits[digest[index] & 0x0F];
  }
  text[40] = 0;
}

}  // namespace zifi
