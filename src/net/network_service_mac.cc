#include "net/network_service.h"

#include "net/kqueue_event_loop.h"

namespace dist_clang {
namespace net {

NetworkService::NetworkService(bool tcp_fast_open)
  : tcp_fast_open_(tcp_fast_open) {
  auto callback = std::bind(&NetworkService::HandleNewConnection, this, _1, _2);
  event_loop_.reset(new KqueueEventLoop(callback));
}

}  // namespace net
}  // namespace dist_clang
