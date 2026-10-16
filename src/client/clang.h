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

// Prints the sizes of the tags of the cache, and how they overlap.
bool PrintTagStats(const String& socket_path,
                   const proto::Configuration& config);

// Removes the |tag| from the cache - or only shows what is removed, unless
// |force|. The empty |tag| removes the entries without tags.
bool ForgetTag(const String& socket_path, const proto::Configuration& config,
               const String& tag, bool force);

}  // namespace client
}  // namespace dist_clang
