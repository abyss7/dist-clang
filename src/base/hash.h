#pragma once

#include "base/hash/murmur_hash3.h"

#include <string>

namespace dist_clang {
namespace base {

inline std::string MakeHash(const std::string& input) {
  char buf[16];
  hash_details::MurmurHash3_x64_128(input.data(), input.size(), 0, buf);
  return std::string(buf, 16);
}

}  // namespace base
}  // namespace dist_clang
