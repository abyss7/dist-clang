#include <net/network_service_impl.h>

#include <net/event_loop_linux.h>

using namespace std::placeholders;

namespace dist_clang {
namespace net {

NetworkServiceImpl::NetworkServiceImpl(ui32 read_timeout_secs,
                                       ui32 send_timeout_secs,
                                       ui32 read_min_bytes,
                                       ui32 connect_timeout_secs)
    : read_timeout_secs_(read_timeout_secs),
      send_timeout_secs_(send_timeout_secs),
      read_min_bytes_(read_min_bytes),
      connect_timeout_secs_(connect_timeout_secs) {
  auto callback =
      std::bind(&NetworkServiceImpl::HandleNewConnection, this, _1, _2);
  event_loop_.reset(new EpollEventLoop(callback));
}

}  // namespace net
}  // namespace dist_clang
