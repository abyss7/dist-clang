#pragma once

#include <cassert>

namespace dist_clang {
namespace base {

inline void Assert(bool expr) {
  assert(expr);
}

}  // namespace base
}  // namespace dist_clang
