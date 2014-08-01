#include <base/hash.h>

#include <third_party/smhasher/exported/MurmurHash3.h>

namespace dist_clang {
namespace base {

String MakeHash(const String& input, ui8 size) {
  char buf[16];
  MurmurHash3_x64_128(input.data(), input.size(), 0, buf);
  return String(buf, std::min<ui8>(16u, size));
}

}  // namespace base
}  // namespace dist_clang
