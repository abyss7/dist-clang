#include <net/event_loop_mac.h>

#include <base/assert.h>
#include <net/connection_impl.h>

#include <sys/event.h>

namespace dist_clang {
namespace net {

KqueueEventLoop::KqueueEventLoop(ConnectionCallback callback)
    : listen_(kqueue()), io_(kqueue()), callback_(callback) {
}

KqueueEventLoop::~KqueueEventLoop() {
  Stop();
}

bool KqueueEventLoop::HandlePassive(Passive&& fd) {
  DCHECK(fd.IsValid());
  auto result = listening_fds_.emplace(std::move(fd));
  DCHECK(result.second);
  return ReadyForListen(*result.first);
}

bool KqueueEventLoop::ReadyForRead(ConnectionImplPtr connection) {
  return ReadyFor(connection, EVFILT_READ);
}

bool KqueueEventLoop::ReadyForSend(ConnectionImplPtr connection) {
  return ReadyFor(connection, EVFILT_WRITE);
}

void KqueueEventLoop::DoListenWork(const Atomic<bool>& is_shutting_down,
                                   base::Data& self) {
  const int MAX_EVENTS = 10;  // This should be enought in most cases.
  struct kevent events[MAX_EVENTS];

  {
    struct kevent event;
    EV_SET(&event, self.native(), EVFILT_READ, EV_ADD, 0, 0, 0);
    kevent(listen_.native(), &event, 1, nullptr, 0, nullptr);
  }

  while (!is_shutting_down) {
    auto events_count =
        kevent(listen_.native(), nullptr, 0, events, MAX_EVENTS, nullptr);
    if (events_count == -1 && errno != EINTR) {
      break;
    }

    for (int i = 0; i < events_count; ++i) {
      if (self.native() == base::Handle::NativeType(events[i].ident)) {
        continue;
      }

      auto* passive = reinterpret_cast<Passive*>(events[i].udata);

      DCHECK(events[i].filter == EVFILT_READ);
      while (true) {
        Socket&& new_fd = passive->Accept();
        if (!new_fd.IsValid()) {
          break;
        }
        DCHECK(new_fd.IsBlocking());

        callback_(*passive, ConnectionImpl::Create(*this, std::move(new_fd)));
      }
      ReadyForListen(*passive);
    }
  }
}

void KqueueEventLoop::DoIOWork(const Atomic<bool>& is_shutting_down,
                               base::Data& self) {
  struct kevent event;
  EV_SET(&event, self.native(), EVFILT_READ, EV_ADD, 0, 0, 0);
  kevent(io_.native(), &event, 1, nullptr, 0, nullptr);

  while (!is_shutting_down) {
    auto events_count = kevent(io_.native(), nullptr, 0, &event, 1, nullptr);
    if (events_count == -1) {
      if (errno != EINTR) {
        break;
      } else {
        continue;
      }
    }

    DCHECK(events_count == 1);
    if (self.native() == base::Handle::NativeType(event.ident)) {
      continue;
    }

    auto raw_connection =
        reinterpret_cast<Connection*>(event.udata)->shared_from_this();
    auto connection = std::static_pointer_cast<ConnectionImpl>(raw_connection);

    if (event.flags & EV_ERROR || (event.flags & EV_EOF && event.data == 0)) {
      ConnectionClose(connection);
    } else if (event.filter == EVFILT_READ) {
      ConnectionDoRead(connection);
    } else if (event.filter == EVFILT_WRITE) {
      ConnectionDoSend(connection);
    } else {
      NOTREACHED();
    }
  }
}

bool KqueueEventLoop::ReadyForListen(const Passive& fd) {
  struct kevent event;
  EV_SET(&event, fd.native(), EVFILT_READ, EV_ADD | EV_ENABLE | EV_ONESHOT, 0,
         0, const_cast<Passive*>(&fd));
  return kevent(listen_.native(), &event, 1, nullptr, 0, nullptr) != -1;
}

bool KqueueEventLoop::ReadyFor(ConnectionImplPtr connection, i16 filter) {
  DCHECK(connection->IsOnEventLoop(this));

  const auto& fd = connection->socket();
  struct kevent event;
  EV_SET(&event, fd.native(), filter, EV_ADD | EV_ONESHOT, 0, 0,
         connection.get());
  return kevent(io_.native(), &event, 1, nullptr, 0, nullptr) != -1;
}

}  // namespace net
}  // namespace dist_clang
