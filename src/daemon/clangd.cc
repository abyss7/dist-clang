#include "server.h"
#include "tclap/CmdLine.h"

#include <iostream>
#include <string>

#ifndef NDEBUG
#include <unistd.h>  // for |pause()|
#endif

using namespace TCLAP;

using std::string;

int main(int argc, char* argv[]) {
  string socket_path;

  try {
    // TODO: use normal versioning.
    CmdLine cmd("Daemon from Clang distributed system - Clangd.", ' ', "0.1");

    // TODO: share the default value string with 'clang'.
    ValueArg<string> socket_arg("s", "socket",
        "Path to UNIX socket to listen for local connections.",
        false, "/tmp/clangd.socket", "path", cmd);
    cmd.parse(argc, argv);
    socket_path = socket_arg.getValue();
  } catch (ArgException &e) {
    std::cerr << "error: " << e.error()
              << " for arg " << e.argId()
              << std::endl;
    return 1;
  }

  string error;
  Server local_listener;
  if (!local_listener.Listen(socket_path, &error)) {
    std::cerr << "Local server: " << error << std::endl;
    return 1;
  }

#ifndef NDEBUG
  pause();
#endif

  return 0;
}
