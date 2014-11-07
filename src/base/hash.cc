#include <base/hash.h>

#include <base/const_string.h>

#include <third_party/smhasher/exported/MurmurHash3.h>

namespace dist_clang {
namespace base {

Immutable MakeHash(Immutable input, ui8 size) {
  char* buf = new char[16];
  MurmurHash3_x64_128(input.data(), input.size(), 0, buf);
  return Immutable(buf, std::min<ui8>(16u, size));
}

}  // namespace base
}  // namespace dist_clang
