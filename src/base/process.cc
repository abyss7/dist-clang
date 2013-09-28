#include "base/process.h"

#include "base/c_utils.h"
#include "net/base/utils.h"

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

Process::Process(const std::string& exec_path, const std::string& cwd_path)
  : exec_path_(exec_path), cwd_path_(cwd_path) {
}

Process& Process::AppendArg(const std::string& arg) {
  args_.push_back(arg);
  return *this;
}

bool Process::Run(unsigned short sec_timeout, std::string* error) {
  int out_pipe_fd[2];
  int err_pipe_fd[2];
  if (pipe(out_pipe_fd) == -1) {
    GetLastError(error);
    return false;
  }
  if (pipe(err_pipe_fd) == -1) {
    GetLastError(error);
    return false;
  }

  int child_pid;
  if ((child_pid = fork()) == 0) {  // Child process.
    close(out_pipe_fd[0]);
    close(err_pipe_fd[0]);
    dup2(out_pipe_fd[1], 1);
    dup2(err_pipe_fd[1], 2);
    close(out_pipe_fd[1]);
    close(err_pipe_fd[1]);

    if (!cwd_path_.empty() && !ChangeCurrentDir(cwd_path_))
      exit(1);

    const char* argv[MAX_ARGS];
    argv[0] = exec_path_.c_str();
    auto arg_it = args_.begin();
    for (size_t i = 1, s = std::min<size_t>(MAX_ARGS, args_.size() + 1);
         i < s;
         ++i, ++arg_it) {
      argv[i] = arg_it->c_str();
    }
    argv[std::min<size_t>(MAX_ARGS, args_.size() + 1)] = nullptr;

    if (execv(exec_path_.c_str(), const_cast<char* const*>(argv)) == -1)
      exit(1);

    return false;
  } else {  // Main process.
    close(out_pipe_fd[1]);
    close(err_pipe_fd[1]);

    int result, status = 0;
    struct pollfd poll_fd[2];
    memset(poll_fd, 0 , sizeof(poll_fd));
    poll_fd[0].fd = out_pipe_fd[0];
    poll_fd[0].events = POLLIN;
    poll_fd[1].fd = err_pipe_fd[0];
    poll_fd[1].events = POLLIN;

    net::MakeNonBlocking(poll_fd[0].fd);
    net::MakeNonBlocking(poll_fd[1].fd);

    const size_t buffer_size = 1024;
    char buffer[buffer_size];
    do {
      if (waitpid(child_pid, &status, WNOHANG) == child_pid)
        break;

      result = poll(poll_fd, 2, sec_timeout * 1000);
      if (result <= 0) {
        if (result == -1)
          GetLastError(error);
        kill();
      }

      if (poll_fd[0].revents == POLLIN) {
        while(true) {
          int bytes_read = read(poll_fd[0].fd, buffer, buffer_size);

          if (!bytes_read || (bytes_read == -1 && errno == EWOULDBLOCK))
            break;
          else if (bytes_read == -1) {
            GetLastError(error);
            kill();
          } else {
            stdout_.append(buffer, bytes_read);
          }
        }
      }
      if (poll_fd[1].revents == POLLIN) {
        while(true) {
          int bytes_read = read(poll_fd[1].fd, buffer, buffer_size);

          if (!bytes_read || (bytes_read == -1 && errno == EWOULDBLOCK))
            break;
          else if (bytes_read <= 0) {
            GetLastError(error);
            kill();
          } else {
            stderr_.append(buffer, bytes_read);
          }
        }
      }
    } while(true);

    while(true) {
      int bytes_read = read(poll_fd[0].fd, buffer, buffer_size);

      if (!bytes_read || (bytes_read == -1 && errno == EWOULDBLOCK))
        break;
      else if (bytes_read == -1) {
        GetLastError(error);
        kill();
      } else {
        stdout_.append(buffer, bytes_read);
      }
    }
    while(true) {
      int bytes_read = read(poll_fd[1].fd, buffer, buffer_size);

      if (!bytes_read || (bytes_read == -1 && errno == EWOULDBLOCK))
        break;
      else if (bytes_read <= 0) {
        GetLastError(error);
        kill();
      } else {
        stderr_.append(buffer, bytes_read);
      }
    }

    close(out_pipe_fd[0]);
    close(err_pipe_fd[0]);

    return !WEXITSTATUS(status);
  }
}

bool Process::Run(unsigned short sec_timeout, const std::string &input,
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

    if (!cwd_path_.empty() && !ChangeCurrentDir(cwd_path_))
      exit(1);

    const char* argv[MAX_ARGS];
    argv[0] = exec_path_.c_str();
    auto arg_it = args_.begin();
    for (size_t i = 1, s = std::min<size_t>(MAX_ARGS, args_.size() + 1);
         i < s;
         ++i, ++arg_it) {
      argv[i] = arg_it->c_str();
    }
    argv[std::min<size_t>(MAX_ARGS, args_.size() + 1)] = nullptr;

    if (execv(exec_path_.c_str(), const_cast<char* const*>(argv)) == -1)
      exit(1);

    return false;
  } else {  // Main process.
    close(in_pipe_fd[0]);
    close(out_pipe_fd[1]);
    close(err_pipe_fd[1]);

    int result, status = 0;
    struct pollfd poll_fd[2];
    memset(poll_fd, 0 , sizeof(poll_fd));
    poll_fd[0].fd = out_pipe_fd[0];
    poll_fd[0].events = POLLIN;
    poll_fd[1].fd = err_pipe_fd[0];
    poll_fd[1].events = POLLIN;

    net::MakeNonBlocking(poll_fd[0].fd);
    net::MakeNonBlocking(poll_fd[1].fd);

    size_t total_sent = 0;
    const size_t buffer_size = 1024;
    char buffer[buffer_size];

    while(total_sent < input.size()) {
      int bytes_sent = write(in_pipe_fd[1], input.data() + total_sent,
                             input.size() - total_sent);
      if (bytes_sent <= 0) {
        GetLastError(error);
        kill();
        break;
      } else {
        total_sent += bytes_sent;
      }
    }
    close(in_pipe_fd[1]);

    do {
      if (waitpid(child_pid, &status, WNOHANG) == child_pid)
        break;

      result = poll(poll_fd, 2, sec_timeout * 1000);
      if (result <= 0) {
        if (result == -1)
          GetLastError(error);
        kill();
      }

      if (poll_fd[0].revents == POLLIN) {
        while(true) {
          int bytes_read = read(poll_fd[0].fd, buffer, buffer_size);

          if (!bytes_read || (bytes_read == -1 && errno == EWOULDBLOCK))
            break;
          else if (bytes_read == -1) {
            GetLastError(error);
            kill();
            break;
          } else {
            stdout_.append(buffer, bytes_read);
          }
        }
      }
      if (poll_fd[1].revents == POLLIN) {
        while(true) {
          int bytes_read = read(poll_fd[1].fd, buffer, buffer_size);

          if (!bytes_read || (bytes_read == -1 && errno == EWOULDBLOCK))
            break;
          else if (bytes_read <= 0) {
            GetLastError(error);
            kill();
            break;
          } else {
            stderr_.append(buffer, bytes_read);
          }
        }
      }
    } while(true);

    while(true) {
      int bytes_read = read(poll_fd[0].fd, buffer, buffer_size);

      if (!bytes_read || (bytes_read == -1 && errno == EWOULDBLOCK))
        break;
      else if (bytes_read == -1) {
        GetLastError(error);
        kill();
      } else {
        stdout_.append(buffer, bytes_read);
      }
    }
    while(true) {
      int bytes_read = read(poll_fd[1].fd, buffer, buffer_size);

      if (!bytes_read || (bytes_read == -1 && errno == EWOULDBLOCK))
        break;
      else if (bytes_read <= 0) {
        GetLastError(error);
        kill();
      } else {
        stderr_.append(buffer, bytes_read);
      }
    }

    close(out_pipe_fd[0]);
    close(err_pipe_fd[0]);

    return !WEXITSTATUS(status) && (total_sent == input.size());
  }
}


void Process::kill() {
  // TODO: implement this.
}

}  // namespace base
}  // namespace dist_clang
