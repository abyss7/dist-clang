#pragma once

#include <base/types.h>

namespace dist_clang {

namespace client {

namespace proto {
class Configuration;
}  // namespace proto

enum class Result {
  SUCCESS,
  FAILURE,
  RUN_LOCALLY,
};

// Sends the compilation to the local daemon and prints its diagnostics.
// |args| is the command line: the compiler followed by its arguments.
Result DoMain(const List<String>& args, const String& socket_path,
              const proto::Configuration& config);

// Prints the daemon's statistics. Returns |false| if the daemon isn't
// available.
bool PrintStats(const String& socket_path, const proto::Configuration& config);

}  // namespace client
}  // namespace dist_clang
