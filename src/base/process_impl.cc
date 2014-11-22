#include <base/process_impl.h>

#include <base/assert.h>
#include <base/c_utils.h>

#include <signal.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

ProcessImpl::ScopedDescriptor::ScopedDescriptor(FileDescriptor fd) : fd_(fd) {
}

ProcessImpl::ScopedDescriptor::~ScopedDescriptor() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

ProcessImpl::ScopedDescriptor::operator FileDescriptor() {
  return fd_;
}

FileDescriptor ProcessImpl::ScopedDescriptor::Release() {
  auto old_fd = fd_;
  fd_ = -1;
  return old_fd;
}

ProcessImpl::ProcessImpl(const String& exec_path, Immutable cwd_path, ui32 uid)
    : Process(exec_path, cwd_path, uid), killed_(false) {
}

// This method contains code between |fork()| and |exec()|. Since we're in a
// multi-threaded program, we have to obey the POSIX recommendations about
// calling only async-signal-safe functions ( see http://goo.gl/kfGvPV ). Also,
// if we use heap-checker then we can't do any heap allocations either, since
// this library may deadlock somewhere inside libunwind.
bool ProcessImpl::RunChild(FileDescriptor(&out_pipe)[2],
                           FileDescriptor(&err_pipe)[2],
                           FileDescriptor* in_pipe) {
  // TODO: replace the std::cerr and std::cout with async-signal-safe analogues.
  // FIXME: replace all this stuff with a call to |posix_spawn()|.
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
    std::cerr << "Can't change current directory to " << cwd_path_.c_str()
              << std::endl;
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

  const char* env[MAX_ARGS];
  auto env_it = envs_.begin();
  for (size_t i = 0, s = envs_.size(); i < s; ++i, ++env_it) {
    env[i] = env_it->c_str();
  }
  DCHECK(env_it == envs_.end());
  env[envs_.size()] = nullptr;

  if ((envs_.size() &&
       execve(exec_path_.c_str(), const_cast<char* const*>(argv),
              const_cast<char* const*>(env)) == -1) ||
      execv(exec_path_.c_str(), const_cast<char* const*>(argv)) == -1) {
    std::cerr << "Failed to execute " << exec_path_.c_str() << ": "
              << strerror(errno) << std::endl;
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
