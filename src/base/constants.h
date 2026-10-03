#pragma once

#include <base/const_string.h>

namespace dist_clang {
namespace base {

extern Literal kEnvClangPath;
extern Literal kEnvClangVersion;
extern Literal kEnvDisabled;
extern Literal kEnvLogLevels;
extern Literal kEnvLogErrorMark;
extern Literal kEnvSocketPath;
extern Literal kEnvNoAutostart;
extern Literal kExtDeps;
extern Literal kExtDwo;
extern Literal kExtManifest;
extern Literal kExtObject;
extern Literal kExtStderr;

// Per-user path of the emitter's socket:
//   $XDG_RUNTIME_DIR/dist-clang.socket or /tmp/dist-clang-<uid>.socket
String DefaultSocketPath();

// Per-user directory for the local cache:
//   $XDG_CACHE_HOME/dist-clang or ~/.cache/dist-clang
String DefaultCacheDir();

// Per-user path of the daemon's configuration file:
//   $XDG_CONFIG_HOME/dist-clang/daemon.conf or ~/.config/dist-clang/daemon.conf
String DefaultDaemonConfigPath();

}  // namespace base
}  // namespace dist_clang
