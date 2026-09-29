// Хост-программа для tests/test_wc_json.py: разбор ответов GitHub и SHA-1.
// Команды со stdin, по одной на строку; ответ — одна строка.
//   ref <файл>   -> "ok <sha>" | "fail"
//   tree <файл>  -> "ok <число> <truncated> [<d|f> <размер> <sha> <путь>]..." | "fail"
//   sha1 <файл>  -> SHA-1 содержимого
//   blob <файл>  -> git-SHA-1 содержимого («blob N\0» + данные)

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "zifi/git_sha1.hpp"
#include "zifi/github_tree.hpp"

namespace {

std::vector<char> readFile(const std::string& path) {
  std::vector<char> data;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return data;
  }
  char buffer[4096];
  size_t count;
  while ((count = std::fread(buffer, 1, sizeof(buffer), file)) != 0) {
    data.insert(data.end(), buffer, buffer + count);
  }
  std::fclose(file);
  return data;
}

}  // namespace

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    const size_t space = line.find(' ');
    const std::string command = line.substr(0, space);
    const std::string path = space == std::string::npos ? "" : line.substr(space + 1);
    const std::vector<char> data = readFile(path);
    if (command == "ref") {
      char commit[41] = {};
      if (zifi::parseGitRefCommit(data.data(), data.size(), commit)) {
        std::cout << "ok " << commit << std::endl;
      } else {
        std::cout << "fail" << std::endl;
      }
    } else if (command == "tree") {
      static zifi::GitTreeEntry entries[64];
      size_t count = 0;
      bool truncated = false;
      if (!zifi::parseGitTree(data.data(), data.size(), entries, 64, count,
                              truncated)) {
        std::cout << "fail" << std::endl;
        continue;
      }
      std::cout << "ok " << count << ' ' << (truncated ? 1 : 0);
      for (size_t index = 0; index < count; ++index) {
        char sha[41];
        zifi::formatSha1Hex(entries[index].sha, sha);
        std::cout << ' ' << (entries[index].directory ? 'd' : 'f') << ' '
                  << entries[index].size << ' ' << sha << ' '
                  << entries[index].path;
      }
      std::cout << std::endl;
    } else if (command == "sha1" || command == "blob") {
      zifi::Sha1 sha;
      if (command == "blob") {
        zifi::gitBlobBegin(sha, static_cast<uint32_t>(data.size()));
      }
      // Кусками разной длины: границы блоков 64 байта не должны влиять.
      size_t offset = 0;
      size_t part = 1;
      while (offset < data.size()) {
        const size_t take = part < data.size() - offset ? part : data.size() - offset;
        sha.update(reinterpret_cast<const uint8_t*>(data.data()) + offset, take);
        offset += take;
        part = part * 3 % 97 + 1;
      }
      uint8_t digest[zifi::Sha1::kDigestSize];
      sha.finish(digest);
      char text[41];
      zifi::formatSha1Hex(digest, text);
      std::cout << text << std::endl;
    } else {
      std::cout << "unknown" << std::endl;
    }
  }
  return 0;
}
