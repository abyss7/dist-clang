#include <net/event_loop_mac.h>

#include <base/assert.h>
#include <net/connection_impl.h>

#include <sys/event.h>

namespace dist_clang {
namespace net {

KqueueEventLoop::KqueueEventLoop(ConnectionCallback callback)
    : callback_(callback) {
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

void KqueueEventLoop::DoListenWork(base::WorkerPool* pool, base::Data& self) {
  std::array<struct kevent, 64> events;

  listen_.Add(self, EVFILT_READ);

  while (!pool->IsShuttingDown()) {
    auto events_count = listen_.Wait(events, base::Kqueue::UNLIMITED);
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

void KqueueEventLoop::DoIOWork(base::WorkerPool* pool, base::Data& self) {
  io_.Add(self, EVFILT_READ);

  std::array<struct kevent, 1> event;

  while (!pool->IsShuttingDown()) {
    auto events_count = io_.Wait(event, base::Kqueue::UNLIMITED);
    if (events_count == -1) {
      if (errno != EINTR) {
        break;
      } else {
        continue;
      }
    }

    DCHECK(events_count == 1);
    if (self.native() == base::Handle::NativeType(event[0].ident)) {
      continue;
    }

    auto raw_connection =
        reinterpret_cast<Connection*>(event[0].udata)->shared_from_this();
    auto connection = std::static_pointer_cast<ConnectionImpl>(raw_connection);

    if (event[0].flags & EV_ERROR ||
        (event[0].flags & EV_EOF && event[0].data == 0)) {
      ConnectionClose(connection);
    } else if (event[0].filter == EVFILT_READ) {
      ConnectionDoRead(connection);
    } else if (event[0].filter == EVFILT_WRITE) {
      ConnectionDoSend(connection);
    } else {
      NOTREACHED();
    }
  }
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
