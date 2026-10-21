#pragma once

#include <base/types.h>
#include <perf/stat.pb.h>

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

// The daemon gets the whole environment of the client: only the daemon of the
// same user, or of root, is trusted - not the one, that took the socket path
// of another user, e.g. in "/tmp".
bool IsTrustedDaemon(ui32 client_uid, ui32 daemon_uid);

// Sends the compilation to the local daemon and prints its diagnostics.
// |args| is the command line: the compiler followed by its arguments.
Result DoMain(const List<String>& args, const String& socket_path,
              const proto::Configuration& config);

// Prints the daemon's statistics - grouped, or as "NAME value" lines with
// |raw|. Returns |false| if the daemon isn't available.
bool PrintStats(const String& socket_path, const proto::Configuration& config,
                bool raw = false);

// Prints the sizes of the tags of the cache, and how they overlap.
bool PrintTagStats(const String& socket_path,
                   const proto::Configuration& config);

// Removes the |tag| from the cache - or only shows what is removed, unless
// |force|. The empty |tag| removes the entries without tags.
bool ForgetTag(const String& socket_path, const proto::Configuration& config,
               const String& tag, bool force);

// The version, the config and the cache of the daemon - for "--stats".
String DaemonStatusText(const perf::proto::DaemonStatus& daemon);

// Stops the daemon, and waits for its exit - the next compilation starts it
// again. Returns |false| if the daemon doesn't stop.
bool StopDaemon(const String& socket_path, const proto::Configuration& config);

// Forgets the records of the removed entries of the cache - and the losses.
bool ForgetGhosts(const String& socket_path,
                  const proto::Configuration& config);

}  // namespace client
}  // namespace dist_clang
