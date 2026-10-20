#include <base/process_impl.h>

#include <base/assert.h>
#include <base/c_utils.h>
#include <base/file/kqueue_mac.h>
#include <base/file/pipe.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/event.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

namespace {

// The duplicate of the descriptor to watch for the disconnection, or for the
// cancellation.
class WatchedHandle : public Handle {
 public:
  // It isn't inherited by the compilers, that the other threads start.
  explicit WatchedHandle(int fd)
      : Handle(fd >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1) {}
};

}  // namespace

// static
int ProcessImpl::CreateCancelEvent() {
  // The own queue with the user event: the queue is readable, when the event
  // is triggered - and is watched by the queue of the running process.
  const int queue = kqueue();
  if (queue == -1) {
    return -1;
  }
  struct kevent event;
  EV_SET(&event, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
  if (kevent(queue, &event, 1, nullptr, 0, nullptr) == -1) {
    close(queue);
    return -1;
  }
  return queue;
}

// static
void ProcessImpl::SignalCancelEvent(int event) {
  struct kevent trigger;
  EV_SET(&trigger, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
  kevent(event, &trigger, 1, nullptr, 0, nullptr);
}

// static
void ProcessImpl::DieWithParent(pid_t) {
  // Unsupported yet: the child outlives the parent.
}

bool ProcessImpl::Run(ui16 sec_timeout, String* error) {
  if (!CheckLimits(error)) {
    return false;
  }

  if (cancel_requested_) {
    canceled_ = true;
    if (error) {
      error->assign("Canceled");
    }
    return false;
  }

  Pipe out, err;
  if (!out.IsValid()) {
    out.GetCreationError(error);
    return false;
  }
  if (!err.IsValid()) {
    err.GetCreationError(error);
    return false;
  }

  int child_pid;
  if ((child_pid = fork()) == 0) {  // Child process.
    return RunChild(out, err, nullptr);
  } else if (child_pid != -1) {  // Main process.
    out[1].Close();
    err[1].Close();

    Kqueue kq;
    if (!kq.IsValid()) {
      kq.GetCreationError(error);
      ::kill(child_pid, SIGTERM);
      return false;
    }
    if (!kq.Add(out[0], EVFILT_READ, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }
    if (!kq.Add(err[0], EVFILT_READ, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }

    // The peer has disconnected: the end of the stream is readable.
    WatchedHandle cancel_socket(cancel_fd_);
    if (cancel_socket.IsValid() &&
        !kq.Add(cancel_socket, EVFILT_READ, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }
    WatchedHandle cancel_event(cancel_event_);
    if (cancel_event.IsValid() && !kq.Add(cancel_event, EVFILT_READ, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }

    size_t stdout_size = 0, stderr_size = 0;
    Immutable::Rope stdout, stderr;
    std::array<struct kevent, 4> events;

    int exhausted_fds = 0;
    while (exhausted_fds < 2 && !killed_) {
      auto event_count = kq.Wait(events, sec_timeout);

      if (event_count == -1) {
        if (errno == EINTR) {
          continue;
        } else {
          GetLastError(error);
          ::kill(child_pid, SIGTERM);
          return false;
        }
      }

      if (event_count == 0) {
        kill(child_pid);
        if (error) {
          error->assign("Timeout occured");
        }
        break;
      }

      for (int i = 0; i < event_count; ++i) {
        if (events[i].udata == &cancel_event) {
          Terminate(child_pid);
          canceled_ = killed_ = true;
          if (error) {
            error->assign("Canceled");
          }
          break;
        }
        if (events[i].udata == &cancel_socket) {
          if (!(events[i].flags & EV_EOF)) {
            // The peer sends something - it's still here: the data stays
            // unread, so don't watch it anymore.
            kq.Delete(cancel_socket);
            continue;
          }
          // The peer is gone - terminate the whole process group.
          Terminate(child_pid);
          canceled_ = killed_ = true;
          if (error) {
            error->assign("Canceled: the peer has disconnected");
          }
          break;
        }

        auto* fd = reinterpret_cast<Data*>(events[i].udata);

        DCHECK(events[i].filter == EVFILT_READ);
        if (events[i].data) {
          auto buffer_size = events[i].data;
          auto buffer = UniquePtr<char[]>(new char[buffer_size]);
          auto bytes_read = read(fd->native(), buffer.get(), buffer_size);
          if (!bytes_read) {
            kq.Delete(*fd);
            exhausted_fds++;
          } else if (bytes_read == -1) {
            GetLastError(error);
            kill(child_pid);
            break;
          } else {
            if (fd == &out[0]) {
              stdout.emplace_back(buffer, bytes_read);
              stdout_size += bytes_read;
            } else {
              stderr.emplace_back(buffer, bytes_read);
              stderr_size += bytes_read;
            }
          }
        } else if (events[i].flags & EV_EOF) {
          kq.Delete(*fd);
          exhausted_fds++;
        } else {
          NOTREACHED();
        }
      }
    }

    stdout_ = Immutable(stdout, stdout_size);
    stderr_ = Immutable(stderr, stderr_size);

    out[0].Close();
    err[0].Close();

    return WaitPid(child_pid, sec_timeout, error) && !killed_;
  } else {  // Failed to fork.
    GetLastError(error);
    return false;
  }
}

bool ProcessImpl::Run(ui16 sec_timeout, Immutable input, String* error) {
  if (!CheckLimits(error)) {
    return false;
  }

  Pipe in, out, err;
  if (!in.IsValid()) {
    in.GetCreationError(error);
    return false;
  }
  if (!out.IsValid()) {
    out.GetCreationError(error);
    return false;
  }
  if (!err.IsValid()) {
    err.GetCreationError(error);
    return false;
  }

  int child_pid;
  if ((child_pid = fork()) == 0) {  // Child process.
    return RunChild(out, err, &in);
  } else if (child_pid != -1) {  // Main process.
    in[0].Close();
    out[1].Close();
    err[1].Close();

    in[1].MakeBlocking(false);

    Kqueue kq;
    if (!kq.IsValid()) {
      kq.GetCreationError(error);
      ::kill(child_pid, SIGTERM);
      return false;
    }

    if (!kq.Add(in[1], EVFILT_WRITE, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }
    if (!kq.Add(out[0], EVFILT_READ, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }
    if (!kq.Add(err[0], EVFILT_READ, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }

    size_t stdin_size = 0, stdout_size = 0, stderr_size = 0;
    Immutable::Rope stdout, stderr;
    std::array<struct kevent, 3> events;

    int exhausted_fds = 0;
    while (exhausted_fds < 3 && !killed_) {
      auto event_count = kq.Wait(events, sec_timeout);

      if (event_count == -1) {
        if (errno == EINTR) {
          continue;
        } else {
          GetLastError(error);
          ::kill(child_pid, SIGTERM);
          return false;
        }
      }

      if (event_count == 0) {
        kill(child_pid);
        if (error) {
          error->assign("Timeout occured");
        }
        break;
      }

      for (int i = 0; i < event_count; ++i) {
        auto* fd = reinterpret_cast<Handle*>(events[i].udata);

        if (events[i].filter == EVFILT_READ && events[i].data) {
          auto buffer_size = events[i].data;
          auto buffer = UniquePtr<char[]>(new char[buffer_size]);
          auto bytes_read = read(fd->native(), buffer.get(), buffer_size);
          if (!bytes_read) {
            kq.Delete(*fd);
            exhausted_fds++;
          } else if (bytes_read == -1) {
            GetLastError(error);
            kill(child_pid);
            break;
          } else {
            if (fd == &out[0]) {
              stdout.emplace_back(buffer, bytes_read);
              stdout_size += bytes_read;
            } else {
              stderr.emplace_back(buffer, bytes_read);
              stderr_size += bytes_read;
            }
          }
        } else if (events[i].filter == EVFILT_WRITE && events[i].data) {
          DCHECK(fd == &in[1]);

          auto bytes_sent = write(fd->native(), input.data() + stdin_size,
                                  input.size() - stdin_size);
          if (bytes_sent < 1) {
            kq.Delete(in[1]);
            in[1].Close();
            exhausted_fds++;
          } else {
            stdin_size += bytes_sent;
            if (stdin_size == input.size()) {
              kq.Delete(in[1]);
              in[1].Close();
              exhausted_fds++;
            }
          }
        } else if (events[i].flags & EV_EOF) {
          kq.Delete(*fd);
          exhausted_fds++;
        }
      }
    }

    stdout_ = Immutable(stdout, stdout_size);
    stderr_ = Immutable(stderr, stderr_size);

    out[0].Close();
    err[0].Close();

    return WaitPid(child_pid, sec_timeout, error) && !killed_;
  } else {  // Failed to fork.
    GetLastError(error);
    return false;
  }
}

}  // namespace base
}  // namespace dist_clang
