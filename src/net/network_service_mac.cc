#include "net/network_service.h"

#include "base/assert.h"
#include "base/c_utils.h"
#include "net/base/end_point.h"
#include "net/base/utils.h"
#include "net/kqueue_event_loop.h"

#include <sys/event.h>

using ::std::string;
using namespace ::std::placeholders;

namespace dist_clang {
namespace net {

bool NetworkService::Run() {
  auto old_signals = BlockSignals();

  poll_fd_ = kqueue();
  auto callback = std::bind(&NetworkService::HandleNewConnection, this, _1, _2);
  event_loop_.reset(new KqueueEventLoop(callback));
  pool_.reset(new WorkerPool);
  auto work = std::bind(&NetworkService::DoConnectWork, this, _1, _2);
  pool_->AddWorker(work, concurrency_);

  UnblockSignals(old_signals);

  return event_loop_->Run();
}

bool NetworkService::ConnectAsync(const EndPointPtr& end_point,
                                  ConnectCallback callback, string *error) {
  auto fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    base::GetLastError(error);
    return false;
  }
  MakeCloseOnExec(fd);
  MakeNonBlocking(fd);

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

  struct kevent event;
  EV_SET(&event, fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, 0);

  std::lock_guard<std::mutex> lock(connect_mutex_);
  if (kevent(poll_fd_, &event, 1, nullptr, 0, nullptr) == -1) {
    base::GetLastError(error);
    close(fd);
    return false;
  }
  auto new_callback = std::make_pair(fd, std::make_pair(callback, end_point));
  connect_callbacks_.insert(new_callback);

  return true;
}

void NetworkService::DoConnectWork(const volatile bool &is_shutting_down,
                                   fd_t self_pipe) {
  const int MAX_EVENTS = 10;  // This should be enought in most cases.
  struct kevent events[MAX_EVENTS];
  unsigned long error;
  unsigned int error_size = sizeof(error);

  {
    struct kevent event;
    EV_SET(&event, self_pipe, EVFILT_READ, EV_ADD, 0, 0, 0);
    kevent(poll_fd_, &event, 1, nullptr, 0, nullptr);
  }

  while(!is_shutting_down) {
    auto events_count =
        kevent(poll_fd_, nullptr, 0, events, MAX_EVENTS, nullptr);
    if (events_count == -1 && errno != EINTR) {
      break;
    }

    for (int i = 0; i < events_count; ++i) {
      fd_t fd = events[i].ident;
      if (fd == self_pipe) {
        continue;
      }

      base::Assert(events[i].filter == EVFILT_WRITE);
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

      EV_SET(events + i, fd, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
      base::Assert(!kevent(poll_fd_, events + i, 1, nullptr, 0, nullptr));
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
