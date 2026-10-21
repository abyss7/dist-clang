#pragma once

#include <base/types.h>

namespace dist_clang {
namespace client {

// What the client is asked to do by its command line.
struct Invocation {
  enum Action {
    USAGE,  // The command line is wrong - or "--help".
    ERROR,  // E.g. the real compiler isn't known: see |error|.
    STATS,
    TAG_STATS,
    FORGET_TAG,
    FORGET_REMOVED,
    COMPILE,
  };

  Action action = USAGE;
  String error;

  // STATS.
  bool raw = false;

  // FORGET_TAG: the empty |tag| is for the untagged entries.
  String tag;
  bool force = false;

  // COMPILE: the options are passed to the daemon with the environment.
  bool restat = false;
  bool recache = false;
  List<String> tags;
  List<String> args;
};

// Parses |argv| of the client. The client runs as a launcher -
// "dist-clang clang++ <args>" - or as the compiler itself, through a symlink
// "clang++" -> "dist-clang": then the real compiler is |compiler_path|.
Invocation ParseInvocation(const List<String>& argv,
                           const String& compiler_path);

}  // namespace client
}  // namespace dist_clang
