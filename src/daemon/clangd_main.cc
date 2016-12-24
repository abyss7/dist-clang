#include <base/c_utils.h>
#include <base/logging.h>
#include <daemon/absorber.h>
#include <daemon/collector.h>
#include <daemon/configuration.h>
#include <daemon/coordinator.h>
#include <daemon/emitter.h>

#include STL(iostream)

#include <signal.h>
#include <unistd.h>

#include <base/using_log.h>

using namespace dist_clang;

namespace {

void signal_handler(int sig) {
  CHECK(sig == SIGSEGV);
  LOG(ERROR) << "Segmentation fault";
  CHECK(false);
}

}  // namespace

int main(int argc, char* argv[]) {
  signal(SIGPIPE, SIG_IGN);
  signal(SIGSEGV, signal_handler);

  daemon::Configuration configuration(argc, argv);
  UniquePtr<daemon::BaseDaemon> daemon, collector, coordinator;

  if (configuration.daemonize()) {
// The function |daemon()| is deprecated on Mac. Use launchd instead.
#if !defined(OS_MACOSX)
    if (::daemon(0, 1) != 0) {
      String error;
      base::GetLastError(&error);
      LOG(FATAL) << "Failed to daemonize: " << error;
    }
#endif  // !defined(OS_MACOSX)

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
    daemon.reset(new daemon::Emitter(configuration.config()));
  } else {
    LOG(FATAL)
        << "Specify exactly one daemon configuration: Absorber or Emitter";
  }

  if (!daemon->Initialize()) {
    LOG(FATAL) << "Daemon failed to initialize.";
  }

  sigset_t signal_mask;
  sigemptyset(&signal_mask);
  sigaddset(&signal_mask, SIGTERM);
  sigaddset(&signal_mask, SIGINT);

  int sig;
  sigwait(&signal_mask, &sig);
  LOG(INFO) << "Received " << sig << " signal before quit";

  return 0;
}
