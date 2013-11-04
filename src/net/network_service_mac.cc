#include "net/network_service.h"

#include "net/kqueue_event_loop.h"

namespace dist_clang {
namespace net {

NetworkService::NetworkService() {
  auto callback = std::bind(&NetworkService::HandleNewConnection, this, _1, _2);
  event_loop_.reset(new KqueueEventLoop(callback));
}

}  // namespace net
}  // namespace dist_clang
