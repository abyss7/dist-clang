#include <base/constants.h>

#include <stdlib.h>
#include <unistd.h>

namespace dist_clang {
namespace base {

Literal kEnvClangPath = "DC_CLANG_PATH"_l;
Literal kEnvTags = "DC_TAGS"_l;
Literal kEnvDisabled = "DC_DISABLED"_l;
Literal kEnvLogLevels = "DC_LOG_LEVELS"_l;
Literal kEnvLogErrorMark = "DC_LOG_ERROR_MARK"_l;
Literal kEnvSocketPath = "DC_SOCKET_PATH"_l;
Literal kEnvNoAutostart = "DC_NO_AUTOSTART"_l;
Literal kExtDeps = "d"_l;
Literal kExtDwo = "dwo"_l;
Literal kExtManifest = "manifest"_l;
Literal kExtObject = "o"_l;
Literal kExtStderr = "stderr"_l;

namespace {

String GetXdgDir(const char* env_name, const char* home_suffix) {
  const char* dir = getenv(env_name);
  if (dir && *dir) {
    return dir;
  }
  const char* home = getenv("HOME");
  return String(home ? home : "/tmp") + home_suffix;
}

}  // namespace

String DefaultSocketPath() {
  const char* runtime_dir = getenv("XDG_RUNTIME_DIR");
  if (runtime_dir && *runtime_dir) {
    return String(runtime_dir) + "/dist-clang.socket";
  }
  return "/tmp/dist-clang-" + std::to_string(getuid()) + ".socket";
}

String DefaultCacheDir() {
  return GetXdgDir("XDG_CACHE_HOME", "/.cache") + "/dist-clang";
}

String DefaultDaemonConfigPath() {
  return GetXdgDir("XDG_CONFIG_HOME", "/.config") + "/dist-clang/daemon.conf";
}

}  // namespace base
}  // namespace dist_clang
