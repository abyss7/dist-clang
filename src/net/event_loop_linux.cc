#include <net/event_loop_linux.h>

#include <base/assert.h>
#include <base/c_utils.h>
#include <net/connection_impl.h>
#include <net/passive.h>

#include <sys/socket.h>

namespace dist_clang {
namespace net {

EpollEventLoop::EpollEventLoop(ConnectionCallback callback)
    : callback_(callback) {
}

EpollEventLoop::~EpollEventLoop() {
  Stop();
}

bool EpollEventLoop::HandlePassive(Passive&& fd) {
  DCHECK(fd.IsValid());
  auto result = listening_fds_.emplace(std::move(fd));
  DCHECK(result.second);
  return ReadyForListen(*result.first);
}

bool EpollEventLoop::ReadyForRead(ConnectionImplPtr connection) {
  return ReadyFor(connection, EPOLLIN);
}

bool EpollEventLoop::ReadyForSend(ConnectionImplPtr connection) {
  return ReadyFor(connection, EPOLLOUT);
}

void EpollEventLoop::DoListenWork(const Atomic<bool>& is_shutting_down,
                                  base::Data& self) {
  std::array<struct epoll_event, 64> events;

  listen_.Add(self, EPOLLIN);

  while (!is_shutting_down) {
    auto events_count = listen_.Wait(events, -1);
    if (events_count == -1 && errno != EINTR) {
      break;
    }

    for (int i = 0; i < events_count; ++i) {
      auto* fd = reinterpret_cast<base::Handle*>(events[i].data.ptr);
      if (fd == &self) {
        continue;
      }

      auto* passive = static_cast<Passive*>(fd);

      DCHECK(events[i].events & EPOLLIN);

      while (true) {
        Socket&& new_fd = passive->Accept();
        if (!new_fd.IsValid()) {
          break;
        }

        callback_(*passive, ConnectionImpl::Create(*this, std::move(new_fd)));
      }
      ReadyForListen(*passive);
    }
  }
}

void EpollEventLoop::DoIOWork(const Atomic<bool>& is_shutting_down,
                              base::Data& self) {
  io_.Add(self, EPOLLIN);

  std::array<struct epoll_event, 1> event;

  while (!is_shutting_down) {
    auto events_count = io_.Wait(event, -1);
    if (events_count == -1) {
      if (errno != EINTR) {
        break;
      } else {
        continue;
      }
    }

    DCHECK(events_count == 1);
    const auto* fd = reinterpret_cast<base::Data*>(event[0].data.ptr);
    if (fd == &self) {
      continue;
    }

    auto raw_connection =
        reinterpret_cast<Connection*>(event[0].data.ptr)->shared_from_this();
    auto connection = std::static_pointer_cast<ConnectionImpl>(raw_connection);
    fd = &connection->socket();

    int data = 0;
    if (event[0].events & EPOLLERR || !fd->ReadyForRead(data) ||
        (event[0].events & EPOLLHUP && data == 0)) {
      ConnectionClose(connection);
    } else if (event[0].events & EPOLLIN) {
      ConnectionDoRead(connection);
    } else if (event[0].events & EPOLLOUT) {
      ConnectionDoSend(connection);
    } else {
      NOTREACHED();
    }
  }
}

bool EpollEventLoop::ReadyFor(ConnectionImplPtr connection, ui32 events) {
  DCHECK(connection->IsOnEventLoop(this));

  const auto& fd = connection->socket();
  struct epoll_event event;
  event.events = events | EPOLLONESHOT;
  event.data.ptr = connection.get();
  return epoll_ctl(io_.native(), EPOLL_CTL_MOD, fd.native(), &event) != -1 ||
         (errno == ENOENT &&
          epoll_ctl(io_.native(), EPOLL_CTL_ADD, fd.native(), &event) != -1);
}

}  // namespace net
}  // namespace dist_clang
