#include <base/process_impl.h>

#include <base/assert.h>
#include <base/file/pipe.h>
#include <base/file_utils.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

ProcessImpl::ProcessImpl(const String& exec_path, const Path& cwd_path,
                         ui32 uid)
    : Process(exec_path, cwd_path, uid), killed_(false) {}

// This method contains code between |fork()| and |exec()|. Since we're in a
// multi-threaded program, we have to obey the POSIX recommendations about
// calling only async-signal-safe functions ( see http://goo.gl/kfGvPV ). Also,
// if we use heap-checker then we can't do any heap allocations either, since
// this library may deadlock somewhere inside libunwind.
bool ProcessImpl::RunChild(Pipe& out, Pipe& err, Pipe* in) {
  // TODO: replace the std::cerr and std::cout with async-signal-safe analogues.
  // FIXME: replace all this stuff with a call to |posix_spawn()|.
  String error;
  if ((in && !(*in)[0].Duplicate(std::move(Handle::stdin()), &error)) ||
      !out[1].Duplicate(std::move(Handle::stdout()), &error) ||
      !err[1].Duplicate(std::move(Handle::stderr()), &error)) {
    std::cerr << "Failed to duplicate: " << error << std::endl;
    exit(1);
  }

  if (in) {
    (*in)[1].Close();
  }
  out[0].Close();
  err[0].Close();

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

bool ProcessImpl::WaitPid(int pid, ui64 sec_timeout, String* error) {
  // TODO: implement killing child on timeout.

  int status;
  int result = waitpid(pid, &status, 0);
  if (result == -1) {
    GetLastError(error);
    return false;
  }

  CHECK(result == pid);

  if (WIFEXITED(status)) {
    return !WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    if (error) {
      std::ostringstream ss;
      ss << "received signal " << WTERMSIG(status);
      *error = ss.str();
    }
    return false;
  } else {
    // The process may have been stopped, but we are not interested in it
    // for now.
    return false;
  }
}

void ProcessImpl::kill(int pid) {
  ::kill(pid, SIGTERM);
  killed_ = true;
}

}  // namespace base
}  // namespace dist_clang
