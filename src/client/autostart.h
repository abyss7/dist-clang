#pragma once

#include <base/types.h>

namespace dist_clang {
namespace client {

// Starts the daemon in background, unless it's already running. The daemon
// executable "dist-clangd" is looked for next to the client's one.
void StartDaemon(const String& socket_path);

}  // namespace client
}  // namespace dist_clang
