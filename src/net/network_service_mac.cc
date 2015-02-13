#include <net/network_service_impl.h>

#include <net/event_loop_mac.h>

using namespace std::placeholders;

namespace dist_clang {
namespace net {

NetworkServiceImpl::NetworkServiceImpl(ui32 read_timeout_secs,
                                       ui32 send_timeout_secs,
                                       ui32 read_min_bytes)
    : read_timeout_secs_(read_timeout_secs),
      send_timeout_secs_(send_timeout_secs),
      read_min_bytes_(read_min_bytes) {
  auto callback =
      std::bind(&NetworkServiceImpl::HandleNewConnection, this, _1, _2);
  event_loop_.reset(new KqueueEventLoop(callback));
}

}  // namespace net
}  // namespace dist_clang
