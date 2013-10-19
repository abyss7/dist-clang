#include "daemon/configuration.h"
#include "daemon/daemon.h"
#include "net/network_service.h"

#include <iostream>  // FIXME: remove when logging will be implemented.

#include <signal.h>

using namespace dist_clang;

int main(int argc, char* argv[]) {
  signal(SIGPIPE, SIG_IGN);

  daemon::Configuration configuration(argc, argv);
  daemon::Daemon daemon;
  net::NetworkService network_service;

  if (!daemon.Initialize(configuration, network_service)) {
    std::cerr << "Daemon failed to initialize." << std::endl;
    return 1;
  }

  if (!network_service.Run()) {
    std::cerr << "Network service failed to run." << std::endl;
    return 1;
  }

  sigset_t signal_mask;
  sigemptyset(&signal_mask);
  sigaddset(&signal_mask, SIGTERM);
  sigaddset(&signal_mask, SIGINT);

  int sig;
  sigwait(&signal_mask, &sig);

  return 0;
}
