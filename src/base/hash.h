#pragma once

#include <base/hash/murmur_hash3.h>
#include <histogram/counter.h>

namespace dist_clang {
namespace base {

inline String MakeHash(const String& input) {
  histogram::Counter counter("hash", input.size());

  char buf[16];
  hash_details::MurmurHash3_x64_128(input.data(), input.size(), 0, buf);
  return String(buf, 16);
}

template <class T, class Hash = std::hash<T>>
inline void HashCombine(std::size_t& seed, const T& value) {
  Hash hash;
  seed ^= hash(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

}  // namespace base
}  // namespace dist_clang
