#include <base/process_impl.h>

#include <base/assert.h>
#include <base/c_utils.h>

#include <signal.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

ProcessImpl::ScopedDescriptor::ScopedDescriptor(net::fd_t fd) : fd_(fd) {}

ProcessImpl::ScopedDescriptor::~ScopedDescriptor() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

ProcessImpl::ScopedDescriptor::operator net::fd_t() { return fd_; }

net::fd_t ProcessImpl::ScopedDescriptor::Release() {
  auto old_fd = fd_;
  fd_ = -1;
  return old_fd;
}

ProcessImpl::ProcessImpl(const std::string& exec_path,
                         const std::string& cwd_path, ui32 uid)
    : Process(exec_path, cwd_path, uid), killed_(false) {}

bool ProcessImpl::RunChild(int (&out_pipe)[2], int (&err_pipe)[2],
                           int* in_pipe) {
  if ((in_pipe && dup2(in_pipe[0], STDIN_FILENO) == -1) ||
      dup2(out_pipe[1], STDOUT_FILENO) == -1 ||
      dup2(err_pipe[1], STDERR_FILENO) == -1) {
    std::cerr << "dup2: " << strerror(errno) << std::endl;
    exit(1);
  }

  if (in_pipe) {
    close(in_pipe[0]);
    close(in_pipe[1]);
  }
  close(out_pipe[0]);
  close(out_pipe[1]);
  close(err_pipe[0]);
  close(err_pipe[1]);

  if (!cwd_path_.empty() && !ChangeCurrentDir(cwd_path_)) {
    std::cerr << "Can't change current directory to " + cwd_path_ << std::endl;
    exit(1);
  }

  if (uid_ != SAME_UID && setuid(uid_) == -1) {
    std::cerr << "Can't set user ID to " + std::to_string(uid_) << std::endl;
    exit(1);
  }

  const char* argv[MAX_ARGS];
  argv[0] = exec_path_.c_str();
  auto arg_it = args_.begin();
  for (size_t i = 1, s = args_.size() + 1; i < s; ++i, ++arg_it) {
    argv[i] = arg_it->c_str();
  }
  DCHECK(arg_it == args_.end());
  argv[args_.size() + 1] = nullptr;

  if (execvp(exec_path_.c_str(), const_cast<char* const*>(argv)) == -1) {
    std::cerr << "execvp: " << strerror(errno) << std::endl;
    exit(1);
  }

  NOTREACHED();
  return false;
}

void ProcessImpl::kill(int pid) {
  ::kill(pid, SIGTERM);
  killed_ = true;
}

}  // namespace base
}  // namespace dist_clang
