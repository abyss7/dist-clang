#pragma once

#include <base/aliases.h>

namespace dist_clang {
namespace base {

String MakeHash(const String& input);

template <class T, class Hash = std::hash<T>>
inline void HashCombine(std::size_t& seed, const T& value) {
  Hash hash;
  seed ^= hash(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

}  // namespace base
}  // namespace dist_clang
