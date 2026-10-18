#pragma once

#include <base/types.h>

namespace dist_clang {
namespace client {

// The flags, that the compiler derives from the |name| of its executable -
// e.g. of the symlink to dist-clang: the real compiler has another name.
List<String> DriverNameArgs(const String& name);

// Removes the outputs of the compilation |args|, before it runs without the
// daemon: they may be the links of the files of the cache, and the compiler
// may write into them in place - e.g. the split DWARF.
void RemoveOutputs(const List<String>& args);

}  // namespace client
}  // namespace dist_clang
