#include "daemon/configuration.h"
#include "daemon/daemon.h"
#include "net/network_service.h"

#include <iostream>  // FIXME: remove when logging will be implemented.

#include <unistd.h>  // for pause()

using namespace dist_clang;

int main(int argc, char* argv[]) {
  daemon::Configuration configuration(argc, argv);
  net::NetworkService network_service;
  daemon::Daemon daemon;

  if (!daemon.Initialize(configuration, network_service)) {
    std::cerr << "Daemon failed to initialize." << std::endl;
    return 1;
  }

  // TODO: implement signal handling. Also, ignore SIGPIPE.
  pause();

  return 0;
}
