#include "base/process.h"

#include "base/assert.h"
#include "base/c_utils.h"

#include <memory>

#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

namespace dist_clang {
namespace base {

bool Process::Run(unsigned sec_timeout, std::string* error) {
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

    ScopedDescriptor epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) {
      GetLastError(error);
      ::kill(child_pid, SIGTERM);
      return false;
    }

    {
      struct epoll_event event;
      event.events = EPOLLIN;
      event.data.fd = out_fd;
      if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, out_fd, &event) == -1) {
        GetLastError(error);
        ::kill(child_pid, SIGTERM);
        return false;
      }
    }
    {
      struct epoll_event event;
      event.events = EPOLLIN;
      event.data.fd = err_fd;
      if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, err_fd, &event) == -1) {
        GetLastError(error);
        ::kill(child_pid, SIGTERM);
        return false;
      }
    }

    size_t stdout_size = 0, stderr_size = 0;
    std::list<std::pair<std::unique_ptr<char[]>, int>> stdout, stderr;
    const int MAX_EVENTS = 2;
    struct epoll_event events[MAX_EVENTS];

    int exhausted_fds = 0;
    while(exhausted_fds < 2 && !killed_) {
      auto event_count =
          epoll_wait(epoll_fd, events, MAX_EVENTS, sec_timeout * 1000);

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
        net::fd_t fd = events[i].data.fd;

        if (events[i].events & EPOLLIN) {
          int bytes_available = 0;
          if (ioctl(fd, FIONREAD, &bytes_available) == -1) {
            GetLastError(error);
            kill(child_pid);
            break;
          }

          auto buffer = std::unique_ptr<char[]>(new char[bytes_available]);
          auto bytes_read = read(fd, buffer.get(), bytes_available);
          if (!bytes_read) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
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
        else {
          epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
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

bool Process::Run(unsigned sec_timeout, const std::string &input,
                  std::string *error) {
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

    ScopedDescriptor epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd == -1) {
      GetLastError(error);
      ::kill(child_pid, SIGTERM);
      return false;
    }

    {
      struct epoll_event event;
      event.events = EPOLLOUT;
      event.data.fd = in_fd;
      if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, in_fd, &event) == -1) {
        GetLastError(error);
        ::kill(child_pid, SIGTERM);
        return false;
      }
    }
    {
      struct epoll_event event;
      event.events = EPOLLIN;
      event.data.fd = out_fd;
      if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, out_fd, &event) == -1) {
        GetLastError(error);
        ::kill(child_pid, SIGTERM);
        return false;
      }
    }
    {
      struct epoll_event event;
      event.events = EPOLLIN;
      event.data.fd = err_fd;
      if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, err_fd, &event) == -1) {
        GetLastError(error);
        ::kill(child_pid, SIGTERM);
        return false;
      }
    }

    size_t stdin_size = 0, stdout_size = 0, stderr_size = 0;
    std::list<std::pair<std::unique_ptr<char[]>, int>> stdout, stderr;
    const int MAX_EVENTS = 3;
    struct epoll_event events[MAX_EVENTS];

    int exhausted_fds = 0;
    while(exhausted_fds < 3 && !killed_) {
      auto event_count =
          epoll_wait(epoll_fd, events, MAX_EVENTS, sec_timeout * 1000);

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
        net::fd_t fd = events[i].data.fd;

        if (events[i].events & EPOLLIN) {
          int bytes_available = 0;
          if (ioctl(fd, FIONREAD, &bytes_available) == -1) {
            GetLastError(error);
            kill(child_pid);
            break;
          }

          auto buffer = std::unique_ptr<char[]>(new char[bytes_available]);
          auto bytes_read = read(fd, buffer.get(), bytes_available);
          if (!bytes_read) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
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
            else if (fd == err_fd) {
              stderr.push_back(std::make_pair(std::move(buffer), bytes_read));
              stderr_size += bytes_read;
            }
          }
        }
        else if (events[i].events & EPOLLOUT) {
          DCHECK(fd == in_fd);

          auto bytes_sent = write(in_fd, input.data() + stdin_size,
                                  input.size() - stdin_size);
          if (bytes_sent < 1) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, in_fd, nullptr);
            exhausted_fds++;
          }
          else {
            stdin_size += bytes_sent;
            if (stdin_size == input.size()) {
              epoll_ctl(epoll_fd, EPOLL_CTL_DEL, in_fd, nullptr);
              close(in_fd.Release());
              exhausted_fds++;
            }
          }
        }
        else {
          epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
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
