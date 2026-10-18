#pragma once

#include <base/const_string.h>
#include <base/types.h>

namespace dist_clang {
namespace base {

inline Path AppendExtension(const Path& path, Literal extension) {
  return path.string().append(Immutable("."_l) + extension);
}

// The absolute normal path of |path| relative to the absolute |dir|, if it's
// relative - like |(dir / path).lexically_normal()|, but much cheaper: the
// lookups of the cache handle thousands of paths.
String NormalPath(const String& dir, const String& path);

}  // namespace base
}  // namespace dist_clang
