#include "daemon/server.h"

#include "base/constants.h"
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
    ValueArg<string> socket_arg("s", "socket",
        "Path to UNIX socket to listen for local connections.",
        false, kDefaultClangdSocket, "path", cmd);
    cmd.parse(argc, argv);

    socket_path = socket_arg.getValue();
  } catch (ArgException &e) {
    std::cerr << "error: " << e.error()
              << " for arg " << e.argId()
              << std::endl;
    return 1;
  }

  string error;
  Server message_handler;
  if (!message_handler.Listen(socket_path, &error)) {
    std::cerr << "Server: " << error << std::endl;
    return 1;
  }

  if (!message_handler.Run()) {
    std::cerr << "Server: failed to start." << std::endl;
    return 1;
  }

  // TODO: setup signals' handler.
  pause();

  return 0;
}
