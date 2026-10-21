#include <base/c_utils.h>
#include <base/file_utils.h>
#include <base/logging.h>
#include <base/platform.h>
#include <daemon/absorber.h>
#include <daemon/collector.h>
#include <daemon/configuration.h>
#include <daemon/coordinator.h>
#include <daemon/emitter.h>

#include STL(iostream)
#include STL(thread)

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>

#include <base/using_log.h>

using namespace dist_clang;

namespace {

void signal_handler(int sig) {
  CHECK(sig == SIGSEGV);
  LOG(ERROR) << "Segmentation fault";
  CHECK(false);
}

// Only one emitter may serve the socket: the lock is held for the whole life of
// the daemon. Returns |false| if another daemon holds it.
bool LockSocket(const daemon::Configuration& configuration) {
  if (configuration.lock_fd() != -1) {
    // The lock is acquired by the client, who has started us.
    return true;
  }

  const String lock_path =
      configuration.config().emitter().socket_path() + ".lock";
  int fd = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd == -1) {
    String error;
    base::GetLastError(&error);
    LOG(FATAL) << "Can't open lock file " << lock_path << ": " << error;
  }
  return flock(fd, LOCK_EX | LOCK_NB) == 0;
}

// The autostarted daemon exits, when its executable is replaced - e.g. by a
// new version: the next client starts the new one. Otherwise the old version
// would work until the user stops it.
void ExitOnUpdate() {
  String path, error;
  if (!base::GetSelfExecutable(path, &error)) {
    LOG(WARNING) << "Can't watch the executable for updates: " << error;
    return;
  }
  const String version = base::FileVersion(path);
  std::thread([path, version] {
    base::SetCurrentThreadName("Update Watcher"_l);
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      // The executable may be missing for a while during the installation.
      const String current = base::FileVersion(path);
      if (!current.empty() && current != version) {
        LOG(INFO) << "The executable " << path << " is updated - exiting";
        kill(getpid(), SIGTERM);
        return;
      }
    }
  }).detach();
}

}  // namespace

int main(int argc, char* argv[]) {
  signal(SIGPIPE, SIG_IGN);
  signal(SIGSEGV, signal_handler);

  // The signals of the exit are waited for below - all threads inherit the
  // mask.
  sigset_t signal_mask;
  sigemptyset(&signal_mask);
  sigaddset(&signal_mask, SIGTERM);
  sigaddset(&signal_mask, SIGINT);
  pthread_sigmask(SIG_BLOCK, &signal_mask, nullptr);

  // Every running compiler takes several descriptors of the daemon, and every
  // waiting client - one more: the soft limit of the client, e.g. 1024, fails
  // the big builds.
  {
    String error;
    if (!base::RaiseOpenFilesLimit(&error)) {
      LOG(WARNING) << "Can't raise the limit of the open files: " << error;
    }
  }

  daemon::Configuration configuration(argc, argv);
  UniquePtr<daemon::BaseDaemon> daemon, collector, coordinator;

  if (configuration.config().has_emitter() && !LockSocket(configuration)) {
    if (configuration.autostart()) {
      return 0;
    }
    LOG(FATAL) << "Another daemon is running on "
               << configuration.config().emitter().socket_path();
  }

  if (configuration.lock_fd() != -1) {
    fcntl(configuration.lock_fd(), F_SETFD, FD_CLOEXEC);
  }

  if (configuration.daemonize()) {
    String error;
    if (!base::Daemonize(&error)) {
      LOG(FATAL) << "Failed to daemonize: " << error;
    }

    base::Log::SetMode(base::Log::SYSLOG);
  }

  if (configuration.config().has_user_id() &&
      setuid(configuration.config().user_id()) == -1) {
    LOG(FATAL) << "Can't run as another user with id "
               << configuration.config().user_id();
  }

  // Initialize collector, if any.
  {
    if (configuration.config().has_collector()) {
      collector.reset(new daemon::Collector(configuration.config()));
    }

    if (collector && !collector->Initialize()) {
      LOG(FATAL) << "Collector failed to initialize.";
    }
  }

  // Initialize coordinator, if any.
  {
    if (configuration.config().has_coordinator()) {
      coordinator.reset(new daemon::Coordinator(configuration.config()));
    }

    if (coordinator && !coordinator->Initialize()) {
      LOG(FATAL) << "Coordinator failed to initialize.";
    }
  }

  if (configuration.config().has_absorber()) {
    daemon.reset(new daemon::Absorber(configuration.config()));
  } else if (configuration.config().has_emitter()) {
    auto* emitter = new daemon::Emitter(configuration.config());
    emitter->SetConfigStatus(configuration.config_path(),
                             configuration.config_error());
    daemon.reset(emitter);
  } else {
    LOG(FATAL)
        << "Specify exactly one daemon configuration: Absorber or Emitter";
  }

  if (!daemon->Initialize()) {
    LOG(FATAL) << "Daemon failed to initialize.";
  }

  if (configuration.autostart()) {
    ExitOnUpdate();
  }

  int sig;
  sigwait(&signal_mask, &sig);
  LOG(INFO) << "Received " << sig << " signal before quit";

  // Don't wait for the running compilations: they are killed with the
  // daemon, and the clients compile without it.
  daemon->Flush();
  _exit(0);
}
