#pragma once

#include <base/types.h>

namespace dist_clang {
namespace client {

// The flags, that the compiler derives from the |name| of its executable -
// e.g. of the symlink to dist-clang: the real compiler has another name.
List<String> DriverNameArgs(const String& name);

}  // namespace client
}  // namespace dist_clang
