#include <client/autostart.h>

#include <base/c_utils.h>
#include <base/file/file.h>
#include <base/logging.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <base/using_log.h>

namespace dist_clang {
namespace client {

void StartDaemon(const String& socket_path) {
  String self_path, error;
  if (!base::GetSelfPath(self_path, &error)) {
    LOG(WARNING) << "Can't find myself to start daemon: " << error;
    return;
  }
  const String daemon_path = self_path + "/dist-clangd";
  if (!base::File::IsExecutable(daemon_path)) {
    LOG(VERBOSE) << "No daemon executable: " << daemon_path;
    return;
  }

  // The running daemon holds the lock all the time. The lock is passed to the
  // new daemon through the inherited file descriptor - so there is no window,
  // when other clients may start another daemon.
  const String lock_path = socket_path + ".lock";
  int lock_fd = open(lock_path.c_str(), O_RDWR | O_CREAT, 0600);
  if (lock_fd == -1) {
    LOG(WARNING) << "Can't open lock file " << lock_path;
    return;
  }
  if (flock(lock_fd, LOCK_EX | LOCK_NB) == -1) {
    // The daemon is running already, or is being started by someone else.
    close(lock_fd);
    return;
  }

  LOG(INFO) << "Starting daemon " << daemon_path;

  const String lock_fd_arg = "--lock-fd=" + std::to_string(lock_fd);
  pid_t pid = fork();
  if (pid == 0) {
    // Detach from the client's session, so that the daemon survives it.
    setsid();
    if (fork() == 0) {
      int null_fd = open("/dev/null", O_RDWR);
      if (null_fd != -1) {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
      }
      execl(daemon_path.c_str(), daemon_path.c_str(), "--autostart",
            lock_fd_arg.c_str(), nullptr);
    }
    _exit(0);
  } else if (pid != -1) {
    waitpid(pid, nullptr, 0);
  }

  close(lock_fd);
}

}  // namespace client
}  // namespace dist_clang
