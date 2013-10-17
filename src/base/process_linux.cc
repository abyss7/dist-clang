#include "base/process.h"

#include "base/assert.h"
#include "base/c_utils.h"

#include <memory>

#include <sys/epoll.h>
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
    if (dup2(out_pipe_fd[1], STDOUT_FILENO) == -1 ||
        dup2(err_pipe_fd[1], STDERR_FILENO) == -1) {
      exit(1);
    }

    close(out_pipe_fd[0]);
    close(err_pipe_fd[0]);
    close(out_pipe_fd[1]);
    close(err_pipe_fd[1]);

    if (!cwd_path_.empty() && !ChangeCurrentDir(cwd_path_)) {
      exit(1);
    }

    base::Assert(args_.size() + 1 < MAX_ARGS);
    const char* argv[MAX_ARGS];
    argv[0] = exec_path_.c_str();
    auto arg_it = args_.begin();
    for (size_t i = 1, s = args_.size() + 1; i < s; ++i, ++arg_it) {
      argv[i] = arg_it->c_str();
    }
    base::Assert(arg_it == args_.end());
    argv[args_.size() + 1] = nullptr;

    if (execv(exec_path_.c_str(), const_cast<char* const*>(argv)) == -1) {
      exit(1);
    }

    return false;
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

    const size_t buffer_size = 10240;
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
        if (error) {
          error->assign("Time-out occured");
        }
        kill(child_pid);
        break;
      }

      for (int i = 0; i < event_count; ++i) {
        if (events[i].events & EPOLLIN) {
          auto buffer = std::unique_ptr<char[]>(new char[buffer_size]);
          auto bytes_read = read(events[i].data.fd, buffer.get(), buffer_size);
          if (!bytes_read) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, nullptr);
            exhausted_fds++;
          }
          else if (bytes_read == -1) {
            GetLastError(error);
            kill(child_pid);
            break;
          }
          else {
            if (events[i].data.fd == out_fd) {
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
          epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, nullptr);
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
    base::Assert(waitpid(child_pid, &status, 0) == child_pid);
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
    if (dup2(in_pipe_fd[0], STDIN_FILENO) == -1 ||
        dup2(out_pipe_fd[1], STDOUT_FILENO) == -1 ||
        dup2(err_pipe_fd[1], STDERR_FILENO) == -1) {
      exit(1);
    }

    close(in_pipe_fd[0]);
    close(out_pipe_fd[0]);
    close(err_pipe_fd[0]);
    close(in_pipe_fd[1]);
    close(out_pipe_fd[1]);
    close(err_pipe_fd[1]);

    if (!cwd_path_.empty() && !ChangeCurrentDir(cwd_path_)) {
      exit(1);
    }

    base::Assert(args_.size() + 1 < MAX_ARGS);
    const char* argv[MAX_ARGS];
    argv[0] = exec_path_.c_str();
    auto arg_it = args_.begin();
    for (size_t i = 1, s = args_.size() + 1; i < s; ++i, ++arg_it) {
      argv[i] = arg_it->c_str();
    }
    base::Assert(arg_it == args_.end());
    argv[args_.size() + 1] = nullptr;

    if (execv(exec_path_.c_str(), const_cast<char* const*>(argv)) == -1) {
      exit(1);
    }

    return false;
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
        if (error) {
          error->assign("Failed to add in_fd to epoll set: " + *error);
        }
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
        if (error) {
          error->assign("Failed to add out_fd to epoll set: " + *error);
        }
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
        if (error) {
          error->assign("Failed to add err_fd to epoll set: " + *error);
        }
        ::kill(child_pid, SIGTERM);
        return false;
      }
    }

    const size_t buffer_size = 10240;
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
        if (error) {
          error->assign("Time-out occured");
        }
        kill(child_pid);
        break;
      }

      for (int i = 0; i < event_count; ++i) {
        if (events[i].events & EPOLLIN) {
          auto buffer = std::unique_ptr<char[]>(new char[buffer_size]);
          auto bytes_read = read(events[i].data.fd, buffer.get(), buffer_size);
          if (!bytes_read) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, nullptr);
            exhausted_fds++;
          }
          else if (bytes_read == -1) {
            GetLastError(error);
            kill(child_pid);
            break;
          }
          else {
            if (events[i].data.fd == out_fd) {
              stdout.push_back(std::make_pair(std::move(buffer), bytes_read));
              stdout_size += bytes_read;
            }
            else if (events[i].data.fd == err_fd) {
              stderr.push_back(std::make_pair(std::move(buffer), bytes_read));
              stderr_size += bytes_read;
            }
          }
        }
        else if (events[i].events & EPOLLOUT) {
          base::Assert(events[i].data.fd == in_fd);

          auto bytes_sent = write(in_fd, input.data() + stdin_size,
                                  input.size() - stdin_size);
          if (!bytes_sent) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, in_fd, nullptr);
            exhausted_fds++;
          }
          else if (bytes_sent == -1) {
            GetLastError(error);
            kill(child_pid);
            break;
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
          epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, nullptr);
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
    base::Assert(waitpid(child_pid, &status, 0) == child_pid);
    return !WEXITSTATUS(status) && !killed_;
  }
  else {  // Failed to fork.
    GetLastError(error);
    return false;
  }
}

}  // namespace base
}  // namespace dist_clang
