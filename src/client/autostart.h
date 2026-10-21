#pragma once

#include <base/types.h>

namespace dist_clang {
namespace client {

// Starts the daemon in background, unless it's already running. The daemon
// executable "dist-clangd" is looked for next to the client's one.
void StartDaemon(const String& socket_path);

// The daemon, that has failed to start, isn't started again for this time, in
// seconds - not for every compilation.
constexpr ui32 kAutostartBackoff = 10;

// If the daemon may be started at the time |now| - with the lock file
// |lock_fd|, that remembers the last start. Sets |warn|, if the failure of the
// start should be reported: once for the backoff.
bool AllowStart(int lock_fd, time_t now, bool* warn);

// Forgets the last start - e.g. after the stop of the daemon: then the next
// compilation starts it at once.
void ForgetStart(int lock_fd);

}  // namespace client
}  // namespace dist_clang
