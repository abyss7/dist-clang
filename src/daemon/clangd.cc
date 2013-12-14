#include "base/logging.h"
#include "daemon/configuration.h"
#include "daemon/daemon.h"
#include "net/network_service.h"

#include <iostream>

#include <signal.h>

#include "base/using_log.h"

using namespace dist_clang;

int main(int argc, char* argv[]) {
  signal(SIGPIPE, SIG_IGN);

  daemon::Configuration configuration(argc, argv);
  daemon::Daemon daemon;

  if (!daemon.Initialize(configuration)) {
    LOG(FATAL) << "Daemon failed to initialize.";
  }

  sigset_t signal_mask;
  sigemptyset(&signal_mask);
  sigaddset(&signal_mask, SIGTERM);
  sigaddset(&signal_mask, SIGINT);

  int sig;
  sigwait(&signal_mask, &sig);
  std::cout << std::endl;

  return 0;
}
