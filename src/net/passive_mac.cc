#include <net/passive.h>

#include <sys/socket.h>

namespace dist_clang {
namespace net {

int Passive::AcceptNative() {
  // The caller makes the socket closed on exec.
  return accept(native(), nullptr, nullptr);
}

}  // namespace net
}  // namespace dist_clang
