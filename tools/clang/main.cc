#include <base/assert.h>
#include <base/c_utils.h>
#include <base/constants.h>
#include <base/logging.h>
#include <client/clang.h>
#include <client/configuration.hh>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <base/using_log.h>

namespace dist_clang {
namespace {

int ExecuteLocally(char* argv[], const String& clang_path) {
  if (clang_path.empty()) {
    LOG(FATAL) << "Provide real clang driver path via " << base::kEnvClangPath;
  }

  LOG(INFO) << "Running locally.";

  if (execv(clang_path.c_str(), argv) == -1) {
    LOG(FATAL) << "Local execution failed: " << strerror(errno);
  }

  NOTREACHED();
  return 1;
}

}  // namespace
}  // namespace dist_clang

int main(int argc, char* argv[]) {
  using namespace dist_clang;

  signal(SIGPIPE, SIG_IGN);

  client::Configuration configuration;
  const auto& config = configuration.config();

  // It's safe to use |base::Log|, since its internal static objects don't need
  // special destruction on |exec|.
  base::Log::RangeSet ranges;
  for (const auto& range : config.verbosity().levels()) {
    ranges.emplace(range.right(), range.left());
  }
  if (!ranges.empty()) {
    base::Log::Reset(config.verbosity().error_mark(), std::move(ranges));
  }

  HashMap<String, String> plugins;
  for (const auto& plugin : config.plugins()) {
    // FIXME: check, if plugin with the same name is not already in the map.
    plugins.emplace(plugin.name(), plugin.path());
    CHECK(plugin.path()[0] == '/') << "Plugin path is not absolute: "
                                   << plugin.path();
  }

  // FIXME: Make default socket path the build param - for consistent packaging.
  Immutable socket_path =
      base::GetEnv(base::kEnvSocketPath, base::kDefaultSocketPath);

  // NOTICE: Use separate |DoMain| function to make sure that all local objects
  //         get destructed before the invokation of |exec|. Do not use global
  //         objects!
  // FIXME: move |configuration| et al. inside |DoMain()| after mocking |GetEnv|
  if (client::DoMain(argc, argv, socket_path,
                     Immutable::WrapString(config.path()),
                     Immutable::WrapString(config.version()),
                     config.connect_timeout(), config.read_timeout(),
                     config.send_timeout(), config.read_minimum(),
                     plugins, config.disabled())) {
    return ExecuteLocally(argv, config.path());
  }

  return 0;
}
