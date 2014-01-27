#include "base/process_impl.h"

#include "base/assert.h"
#include "base/c_utils.h"
#include "net/base/utils.h"

#include <signal.h>
#include <sys/event.h>

namespace dist_clang {
namespace base {

bool ProcessImpl::Run(unsigned sec_timeout, std::string* error) {
  CHECK(args_.size() + 1 < MAX_ARGS);

  int out_pipe_fd[2];
  int err_pipe_fd[2];
  if (pipe(out_pipe_fd) == -1) {
    GetLastError(error);
    return false;
  }
  if (pipe(err_pipe_fd) == -1) {
    GetLastError(error);
    close(out_pipe_fd[0]);
    close(out_pipe_fd[1]);
    return false;
  }

  int child_pid;
  if ((child_pid = fork()) == 0) {  // Child process.
    return RunChild(out_pipe_fd, err_pipe_fd, nullptr);
  }
  else if (child_pid != -1) {  // Main process.
    close(out_pipe_fd[1]);
    close(err_pipe_fd[1]);
    ScopedDescriptor out_fd(out_pipe_fd[0]);
    ScopedDescriptor err_fd(err_pipe_fd[0]);

    ScopedDescriptor kq_fd(kqueue());
    if (kq_fd == -1) {
      GetLastError(error);
      ::kill(child_pid, SIGTERM);
      return false;
    }

    const int MAX_EVENTS = 2;
    struct kevent events[MAX_EVENTS];
    EV_SET(events + 0, out_fd, EVFILT_READ, EV_ADD, 0, 0, 0);
    EV_SET(events + 1, err_fd, EVFILT_READ, EV_ADD, 0, 0, 0);
    if (kevent(kq_fd, events, MAX_EVENTS, nullptr, 0, nullptr) == -1) {
      GetLastError(error);
      ::kill(child_pid, SIGTERM);
      return false;
    }

    struct timespec timeout = { sec_timeout, 0 };
    struct timespec* timeout_ptr = nullptr;
    if (sec_timeout != UNLIMITED) {
      timeout_ptr = &timeout;
    }
    size_t stdout_size = 0, stderr_size = 0;
    std::list<std::pair<std::unique_ptr<char[]>, int>> stdout, stderr;

    int exhausted_fds = 0;
    while(exhausted_fds < 2 && !killed_) {
      auto event_count =
          kevent(kq_fd, nullptr, 0, events, MAX_EVENTS, timeout_ptr);

      if (event_count == -1) {
        if (errno == EINTR) {
          continue;
        }
        else {
          ::kill(child_pid, SIGTERM);
          return false;
        }
      }

      if (event_count == 0) {
        kill(child_pid);
        break;
      }

      for (int i = 0; i < event_count; ++i) {
        net::fd_t fd = events[i].ident;
        if (events[i].filter == EVFILT_READ && events[i].data) {
          auto buffer_size = events[i].data;
          auto buffer = std::unique_ptr<char[]>(new char[buffer_size]);
          auto bytes_read = read(fd, buffer.get(), buffer_size);
          if (!bytes_read) {
            EV_SET(events + i, fd, EVFILT_READ, EV_DELETE, 0, 0, 0);
            kevent(kq_fd, events + i, 1, nullptr, 0, nullptr);
            exhausted_fds++;
          }
          else if (bytes_read == -1) {
            GetLastError(error);
            kill(child_pid);
            break;
          }
          else {
            if (fd == out_fd) {
              stdout.push_back(std::make_pair(std::move(buffer), bytes_read));
              stdout_size += bytes_read;
            }
            else {
              stderr.push_back(std::make_pair(std::move(buffer), bytes_read));
              stderr_size += bytes_read;
            }
          }
        }
        else if (events[i].filter == EVFILT_READ && events[i].flags & EV_EOF) {
          EV_SET(events + i, fd, EVFILT_READ, EV_DELETE, 0, 0, 0);
          kevent(kq_fd, events + i, 1, nullptr, 0, nullptr);
          exhausted_fds++;
        }
      }
    }

    stdout_.reserve(stdout_size);
    for (const auto& piece: stdout) {
      stdout_.append(std::string(piece.first.get(), piece.second));
    }

    stderr_.reserve(stderr_size);
    for (const auto& piece: stderr) {
      stderr_.append(std::string(piece.first.get(), piece.second));
    }

    int status;
    CHECK(waitpid(child_pid, &status, 0) == child_pid);
    return !WEXITSTATUS(status) && !killed_;
  }
  else {  // Failed to fork.
    GetLastError(error);
    return false;
  }
}

bool ProcessImpl::Run(unsigned sec_timeout, const std::string& input,
                      std::string* error) {
  CHECK(args_.size() + 1 < MAX_ARGS);

  int in_pipe_fd[2];
  int out_pipe_fd[2];
  int err_pipe_fd[2];
  if (pipe(in_pipe_fd) == -1) {
    GetLastError(error);
    return false;
  }
  if (pipe(out_pipe_fd) == -1) {
    GetLastError(error);
    close(in_pipe_fd[0]);
    close(in_pipe_fd[1]);
    return false;
  }
  if (pipe(err_pipe_fd) == -1) {
    GetLastError(error);
    close(in_pipe_fd[0]);
    close(in_pipe_fd[1]);
    close(out_pipe_fd[0]);
    close(out_pipe_fd[1]);
    return false;
  }

  int child_pid;
  if ((child_pid = fork()) == 0) {  // Child process.
    return RunChild(out_pipe_fd, err_pipe_fd, in_pipe_fd);
  }
  else if (child_pid != -1) {  // Main process.
    close(in_pipe_fd[0]);
    close(out_pipe_fd[1]);
    close(err_pipe_fd[1]);
    ScopedDescriptor in_fd(in_pipe_fd[1]);
    ScopedDescriptor out_fd(out_pipe_fd[0]);
    ScopedDescriptor err_fd(err_pipe_fd[0]);

    net::MakeNonBlocking(in_fd);

    ScopedDescriptor kq_fd(kqueue());
    if (kq_fd == -1) {
      GetLastError(error);
      ::kill(child_pid, SIGTERM);
      return false;
    }

    const int MAX_EVENTS = 3;
    struct kevent events[MAX_EVENTS];
    EV_SET(events + 0, in_fd, EVFILT_WRITE, EV_ADD, 0, 0, 0);
    EV_SET(events + 1, out_fd, EVFILT_READ, EV_ADD, 0, 0, 0);
    EV_SET(events + 2, err_fd, EVFILT_READ, EV_ADD, 0, 0, 0);
    if (kevent(kq_fd, events, MAX_EVENTS, nullptr, 0, nullptr) == -1) {
      GetLastError(error);
      ::kill(child_pid, SIGTERM);
      return false;
    }

    struct timespec timeout = { sec_timeout, 0 };
    struct timespec* timeout_ptr = nullptr;
    if (sec_timeout != UNLIMITED) {
      timeout_ptr = &timeout;
    }
    size_t stdin_size = 0, stdout_size = 0, stderr_size = 0;
    std::list<std::pair<std::unique_ptr<char[]>, int>> stdout, stderr;

    int exhausted_fds = 0;
    while(exhausted_fds < 3 && !killed_) {
      auto event_count =
          kevent(kq_fd, nullptr, 0, events, MAX_EVENTS, timeout_ptr);

      if (event_count == -1) {
        if (errno == EINTR) {
          continue;
        }
        else {
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
        net::fd_t fd = events[i].ident;

        if (events[i].filter == EVFILT_READ && events[i].data) {
          auto buffer_size = events[i].data;
          auto buffer = std::unique_ptr<char[]>(new char[buffer_size]);
          auto bytes_read = read(fd, buffer.get(), buffer_size);
          if (!bytes_read) {
            EV_SET(events + i, fd, EVFILT_READ, EV_DELETE, 0, 0, 0);
            kevent(kq_fd, events + i, 1, nullptr, 0, nullptr);
            exhausted_fds++;
          }
          else if (bytes_read == -1) {
            GetLastError(error);
            kill(child_pid);
            break;
          }
          else {
            if (fd == out_fd) {
              stdout.push_back(std::make_pair(std::move(buffer), bytes_read));
              stdout_size += bytes_read;
            }
            else {
              stderr.push_back(std::make_pair(std::move(buffer), bytes_read));
              stderr_size += bytes_read;
            }
          }
        }
        else if (events[i].filter == EVFILT_WRITE && events[i].data) {
          DCHECK(fd == in_fd);

          auto bytes_sent = write(fd, input.data() + stdin_size,
                                  input.size() - stdin_size);
          if (bytes_sent < 1) {
            EV_SET(events + i, fd, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
            kevent(kq_fd, events + i, 1, nullptr, 0, nullptr);
            exhausted_fds++;
          }
          else {
            stdin_size += bytes_sent;
            if (stdin_size == input.size()) {
              EV_SET(events + i, fd, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
              kevent(kq_fd, events + i, 1, nullptr, 0, nullptr);
              close(in_fd.Release());
              exhausted_fds++;
            }
          }
        }
        else if (events[i].flags & EV_EOF) {
          EV_SET(events + i, fd, events[i].filter, EV_DELETE, 0, 0, 0);
          kevent(kq_fd, events + i, 1, nullptr, 0, nullptr);
          exhausted_fds++;
        }
      }
    }

    stdout_.reserve(stdout_size);
    for (const auto& piece: stdout) {
      stdout_.append(std::string(piece.first.get(), piece.second));
    }

    stderr_.reserve(stderr_size);
    for (const auto& piece: stderr) {
      stderr_.append(std::string(piece.first.get(), piece.second));
    }

    int status;
    CHECK(waitpid(child_pid, &status, 0) == child_pid);
    return !WEXITSTATUS(status) && !killed_;
  }
  else {  // Failed to fork.
    GetLastError(error);
    return false;
  }
}

}  // namespace base
}  // namespace dist_clang
