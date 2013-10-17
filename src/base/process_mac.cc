#include "base/process.h"

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

    // TODO: use kqueue.

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
