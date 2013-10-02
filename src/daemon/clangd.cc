#include "daemon/configuration.h"
#include "daemon/daemon.h"
#include "net/network_service.h"

#include <iostream>  // FIXME: remove when logging will be implemented.

#include <unistd.h>  // for pause()

using namespace dist_clang;

int main(int argc, char* argv[]) {
  daemon::Configuration configuration(argc, argv);
  daemon::Daemon daemon;
  net::NetworkService network_service;

  if (!daemon.Initialize(configuration, network_service)) {
    std::cerr << "Daemon failed to initialize." << std::endl;
    return 1;
  }

  // TODO: implement signal handling.
  signal(SIGPIPE, SIG_IGN);
  pause();

  return 0;
}
