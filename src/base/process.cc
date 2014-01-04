#include "base/process.h"

#include "base/assert.h"
#include "base/c_utils.h"
#include "proto/remote.pb.h"

#include <signal.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

Process::ScopedDescriptor::ScopedDescriptor(net::fd_t fd)
  : fd_(fd) {
}

Process::ScopedDescriptor::~ScopedDescriptor() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

Process::ScopedDescriptor::operator net::fd_t () {
  return fd_;
}

net::fd_t Process::ScopedDescriptor::Release() {
  auto old_fd = fd_;
  fd_ = -1;
  return old_fd;
}

Process::Process(const std::string& exec_path, const std::string& cwd_path)
  : exec_path_(exec_path), cwd_path_(cwd_path), killed_(false) {
}

Process::Process(const proto::Flags &flags, const std::string &cwd_path)
  : Process(flags.compiler().path(), cwd_path) {
  // |flags.other()| always must go first, since they contain "-cc1" flag.
  AppendArg(flags.other().begin(), flags.other().end());
  AppendArg(flags.non_cached().begin(), flags.non_cached().end());
  AppendArg(flags.dependenies().begin(), flags.dependenies().end());
  for (const auto& plugin: flags.compiler().plugins()) {
    AppendArg("-load").AppendArg(plugin.path());
  }
  if (flags.has_language()) {
    AppendArg("-x").AppendArg(flags.language());
  }
  if (flags.has_output()) {
    AppendArg("-o").AppendArg(flags.output());
  }
  if (flags.has_input()) {
    AppendArg(flags.input());
  }
}

Process& Process::AppendArg(const std::string& arg) {
  args_.push_back(arg);
  return *this;
}

bool Process::RunChild(int (&out_pipe)[2], int (&err_pipe)[2], int* in_pipe) {
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

  CHECK(args_.size() + 1 < MAX_ARGS);
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

void Process::kill(int pid) {
  ::kill(pid, SIGTERM);
  killed_ = true;
}

}  // namespace base
}  // namespace dist_clang
