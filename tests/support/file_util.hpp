#pragma once

#include "strata/env.hpp"

#include <string>

namespace strata::test {

// Reads a whole file through the default Env; returns "" if it cannot be read.
//
// Tests use this instead of std::ifstream + istreambuf_iterator, which GCC 13
// flags with a -Wnull-dereference false positive inside libstdc++.
inline std::string ReadFileToString(const std::string& path) {
  std::string contents;
  auto file = Env::Default()->NewSequentialFile(path);
  if (!file.ok()) {
    return contents;
  }
  std::string scratch(64 * 1024, '\0');
  while (true) {
    auto chunk = (*file)->Read(scratch.size(), scratch.data());
    if (!chunk.ok() || chunk->empty()) {
      break;
    }
    contents.append(*chunk);
  }
  return contents;
}

}  // namespace strata::test
