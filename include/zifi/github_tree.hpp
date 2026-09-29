#pragma once

#include <stddef.h>
#include <stdint.h>

#include "zifi/git_sha1.hpp"

namespace zifi {

// Запись дерева git из ответа GitHub API. Путь — относительно запрошенного
// каталога, с '/' между частями, в UTF-8.
struct GitTreeEntry {
  static constexpr size_t kPathSize = 128;
  char path[kPathSize] = {};
  uint8_t sha[Sha1::kDigestSize] = {};
  uint32_t size = 0;
  bool directory = false;
};

// GET /repos/{repo}/git/ref/heads/{branch}:
// {"ref":"refs/heads/main","object":{"sha":"<40 hex>","type":"commit",...}}
bool parseGitRefCommit(const char* json, size_t length, char commit[41]);

// GET /repos/{repo}/git/trees/{commit}:{dir}?recursive=1:
// {"sha":...,"tree":[{"path":...,"type":"blob","sha":...,"size":N},...],
//  "truncated":false}. Берутся записи "blob" и "tree"; подмодули ("commit")
// пропускаются. Слишком длинный путь или лишняя запись — отказ целиком:
// неполный список нельзя выдавать за эталон.
bool parseGitTree(const char* json, size_t length, GitTreeEntry* entries,
                  size_t capacity, size_t& count, bool& truncated);

}  // namespace zifi
