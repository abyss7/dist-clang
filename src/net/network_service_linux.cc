#include "net/network_service.h"

#include "base/assert.h"
#include "base/c_utils.h"
#include "net/base/end_point.h"
#include "net/base/utils.h"
#include "net/epoll_event_loop.h"

#include <sys/epoll.h>
#include <sys/socket.h>

using ::std::string;
using namespace ::std::placeholders;

namespace dist_clang {
namespace net {

bool NetworkService::Run() {
  auto old_signals = BlockSignals();

  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  auto callback = std::bind(&NetworkService::HandleNewConnection, this, _1, _2);
  event_loop_.reset(new EpollEventLoop(callback));
  pool_.reset(new WorkerPool);
  auto work = std::bind(&NetworkService::DoConnectWork, this, _1);
  pool_->AddWorker(work, concurrency_);

  UnblockSignals(old_signals);

  return event_loop_->Run();
}

bool NetworkService::ConnectAsync(const EndPointPtr& end_point,
                                  ConnectCallback callback, string *error) {
  auto fd = socket(AF_INET, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0);
  if (fd == -1) {
    base::GetLastError(error);
    return false;
  }

  auto res = connect(fd, *end_point, end_point->size());
  if (res == -1) {
    if (errno != EINPROGRESS) {
      base::GetLastError(error);
      close(fd);
      return false;
    }
  }
  else if (res == 0) {
    MakeNonBlocking(fd, true);
    callback(Connection::Create(*event_loop_, fd, end_point), string());
    return true;
  }

  struct epoll_event event;
  event.events = EPOLLOUT | EPOLLONESHOT;
  event.data.fd = fd;
  std::lock_guard<std::mutex> lock(connect_mutex_);
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == -1) {
    base::GetLastError(error);
    close(fd);
    return false;
  }
  auto new_callback = std::make_pair(fd, std::make_pair(callback, end_point));
  connect_callbacks_.insert(new_callback);

  return true;
}

void NetworkService::DoConnectWork(const volatile bool &is_shutting_down) {
  const int MAX_EVENTS = 10;  // This should be enought in most cases.
  struct epoll_event events[MAX_EVENTS];
  sigset_t signal_set;
  unsigned long error;
  unsigned int error_size = sizeof(error);

  sigfillset(&signal_set);
  sigdelset(&signal_set, WorkerPool::interrupt_signal);
  while(!is_shutting_down) {
    auto events_count =
        epoll_pwait(epoll_fd_, events, MAX_EVENTS, -1, &signal_set);
    if (events_count == -1 && errno != EINTR) {
      break;
    }

    for (int i = 0; i < events_count; ++i) {
      base::Assert(events[i].events & EPOLLOUT);
      fd_t fd = events[i].data.fd;
      ConnectCallback callback;
      EndPointPtr end_point;
      {
        std::lock_guard<std::mutex> lock(connect_mutex_);
        auto it = connect_callbacks_.find(fd);
        base::Assert(it != connect_callbacks_.end());
        callback = it->second.first;
        end_point = it->second.second;
        connect_callbacks_.erase(it);
      }

      base::Assert(!epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr));
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) == -1) {
        string error;
        base::GetLastError(&error);
        callback(ConnectionPtr(), error);
        close(fd);
      }
      else if (error) {
        errno = error;
        string error;
        base::GetLastError(&error);
        callback(ConnectionPtr(), error);
        close(fd);
      }
      else {
        MakeNonBlocking(fd, true);
        callback(Connection::Create(*event_loop_, fd, end_point), string());
      }
    }
  }
}

}  // namespace net
}  // namespace dist_clang
