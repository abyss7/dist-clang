#include <base/process_impl.h>

#include <base/assert.h>
#include <base/c_utils.h>
#include <base/file/epoll_linux.h>
#include <base/file/pipe.h>
#include <base/logging.h>

#include <signal.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <base/using_log.h>

namespace dist_clang {
namespace base {

namespace {

// The duplicate of the socket to watch for the disconnection.
class WatchedSocket : public Handle {
 public:
  explicit WatchedSocket(int fd) : Handle(fd >= 0 ? dup(fd) : -1) {}
};

}  // namespace

// static
int ProcessImpl::CreateCancelEvent() {
  return eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
}

// static
void ProcessImpl::SignalCancelEvent(int event) {
  const ui64 value = 1;
  [[maybe_unused]] auto result = write(event, &value, sizeof(value));
}

// static
void ProcessImpl::DieWithParent() {
  const pid_t parent = getppid();
  prctl(PR_SET_PDEATHSIG, SIGKILL);
  // The parent may be gone already.
  if (getppid() != parent) {
    _exit(1);
  }
}

bool ProcessImpl::Run(ui16 sec_timeout, String* error) {
  CHECK(args_.size() + 1 < MAX_ARGS);

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

  LOG(VERBOSE) << "Running process: " << exec_path_ << " " << args_;

  int child_pid;
  if ((child_pid = fork()) == 0) {  // Child process.
    return RunChild(out, err, nullptr);
  } else if (child_pid != -1) {  // Main process.
    out[1].Close();
    err[1].Close();

    Epoll epoll;
    if (!epoll.IsValid()) {
      epoll.GetCreationError(error);
      ::kill(child_pid, SIGTERM);
      return false;
    }
    if (!epoll.Add(out[0], EPOLLIN, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }
    if (!epoll.Add(err[0], EPOLLIN, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }

    WatchedSocket cancel_socket(cancel_fd_);
    if (cancel_socket.IsValid() &&
        !epoll.Add(cancel_socket, EPOLLRDHUP, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }
    WatchedSocket cancel_event(cancel_event_);
    if (cancel_event.IsValid() && !epoll.Add(cancel_event, EPOLLIN, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }

    size_t stdout_size = 0, stderr_size = 0;
    Immutable::Rope stdout, stderr;
    std::array<struct epoll_event, 4> events;

    int epoll_timeout = sec_timeout == UNLIMITED ? -1 : sec_timeout * 1000;
    int exhausted_fds = 0;
    while (exhausted_fds < 2 && !killed_) {
      auto event_count = epoll.Wait(events, epoll_timeout);

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
        if (events[i].data.ptr == &cancel_event) {
          Terminate(child_pid);
          canceled_ = killed_ = true;
          if (error) {
            error->assign("Canceled");
          }
          break;
        }
        if (events[i].data.ptr == &cancel_socket) {
          // The peer is gone - terminate the whole process group.
          Terminate(child_pid);
          canceled_ = killed_ = true;
          if (error) {
            error->assign("Canceled: the peer has disconnected");
          }
          break;
        }

        auto* fd = reinterpret_cast<Data*>(events[i].data.ptr);

        if (events[i].events & EPOLLIN) {
          Immutable buffer;
          if (!fd->Read(&buffer, error)) {
            kill(child_pid);
            break;
          }

          if (buffer.empty()) {
            epoll.Delete(*fd);
            exhausted_fds++;
          } else {
            if (fd == &out[0]) {
              stdout_size += buffer.size();
              stdout.emplace_back(buffer);
            } else {
              stderr_size += buffer.size();
              stderr.emplace_back(buffer);
            }
          }
        } else {
          epoll.Delete(*fd);
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

bool ProcessImpl::Run(ui16 sec_timeout, Immutable input, String* error) {
  CHECK(args_.size() + 1 < MAX_ARGS);

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

    Epoll epoll;
    if (!epoll.IsValid()) {
      epoll.GetCreationError(error);
      ::kill(child_pid, SIGTERM);
      return false;
    }

    if (!epoll.Add(in[1], EPOLLOUT, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }
    if (!epoll.Add(out[0], EPOLLIN, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }
    if (!epoll.Add(err[0], EPOLLIN, error)) {
      ::kill(child_pid, SIGTERM);
      return false;
    }

    size_t stdin_size = 0, stdout_size = 0, stderr_size = 0;
    Immutable::Rope stdout, stderr;
    std::array<struct epoll_event, 3> events;

    int epoll_timeout = sec_timeout == UNLIMITED ? -1 : sec_timeout * 1000;
    int exhausted_fds = 0;
    while (exhausted_fds < 3 && !killed_) {
      auto event_count = epoll.Wait(events, epoll_timeout);

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
        auto* fd = reinterpret_cast<Data*>(events[i].data.ptr);

        if (events[i].events & EPOLLIN) {
          Immutable buffer;
          if (!fd->Read(&buffer, error)) {
            kill(child_pid);
            break;
          }

          if (buffer.empty()) {
            epoll.Delete(*fd);
            exhausted_fds++;
          } else {
            if (fd == &out[0]) {
              stdout_size += buffer.size();
              stdout.emplace_back(buffer);
            } else {
              stderr_size += buffer.size();
              stderr.emplace_back(buffer);
            }
          }
        } else if (events[i].events & EPOLLOUT) {
          DCHECK(fd == &in[1]);

          auto bytes_sent = write(fd->native(), input.data() + stdin_size,
                                  input.size() - stdin_size);
          if (bytes_sent < 1) {
            epoll.Delete(*fd);
            fd->Close();
            exhausted_fds++;
          } else {
            stdin_size += bytes_sent;
            if (stdin_size == input.size()) {
              epoll.Delete(*fd);
              fd->Close();
              exhausted_fds++;
            }
          }
        } else {
          epoll.Delete(*fd);
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
