#include "net/network_service.h"

#include "net/epoll_event_loop.h"

using namespace std::placeholders;

namespace dist_clang {
namespace net {

NetworkService::NetworkService() {
  auto callback = std::bind(&NetworkService::HandleNewConnection, this, _1, _2);
  event_loop_.reset(new EpollEventLoop(callback));
}

}  // namespace net
}  // namespace dist_clang
