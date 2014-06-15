#include <net/network_service_impl.h>

#include <net/kqueue_event_loop.h>

using namespace std::placeholders;

namespace dist_clang {
namespace net {

NetworkServiceImpl::NetworkServiceImpl() {
  auto callback =
      std::bind(&NetworkServiceImpl::HandleNewConnection, this, _1, _2);
  event_loop_.reset(new KqueueEventLoop(callback));
}

}  // namespace net
}  // namespace dist_clang
