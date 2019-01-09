#pragma once

#include <base/types.h>

namespace dist_clang {
namespace base {

inline Path AppendExtension(const Path& path, Literal extension) {
  return path.string().append(Immutable("."_l) + extension);
}

}  // namespace base
}  // namespace dist_clang
