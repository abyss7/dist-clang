#include <net/network_service_impl.h>

#include <base/c_utils.h>
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

NetworkServiceImpl::ConnectedStatus
NetworkServiceImpl::WaitForConnection(const base::Handle& fd, String* error) {
  base::Epoll waiter;
  if (!waiter.IsValid()) {
    waiter.GetCreationError(error);
    return ConnectedStatus::FAILED;
  }

  if (!waiter.Add(fd, EPOLLOUT | EPOLLONESHOT, error)) {
    return ConnectedStatus::FAILED;
  }

  std::array<struct epoll_event, 1> event;
  int timeout = connect_timeout_secs_ ? connect_timeout_secs_ * 1000 : -1;
  auto events_count = waiter.Wait(event, timeout);
  // TODO: handle EINTR?
  if (events_count == -1) {
    base::GetLastError(error);
    return ConnectedStatus::FAILED;
  }

  DCHECK(events_count < 2);
  return events_count == 1 ? ConnectedStatus::CONNECTED :
                             ConnectedStatus::TIMED_OUT;
}

}  // namespace net
}  // namespace dist_clang
