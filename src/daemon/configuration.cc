#include <daemon/configuration.h>

#include <base/constants.h>
#include <base/file/file.h>
#include <base/logging.h>
#include <base/protobuf_utils.h>

#include STL(iostream)
#include STL(thread)

#include <base/using_log.h>

namespace dist_clang {
namespace daemon {

namespace {

const char kUsage[] =
    "Usage: dist-clangd [--config=<path>] [--daemon]\n"
    "       dist-clangd --autostart [--socket=<path>]\n"
    "\n"
    "  --config=<path>  Path to the configuration file\n"
    "  --daemon         Daemonize after start\n"
    "  --autostart      Run the local emitter in background - with the config\n"
    "                   from the default path, if exists, or with defaults\n"
    "  --socket=<path>  The socket of the emitter instead of the configured\n"
    "                   one - e.g. the one, that the client has locked\n"
    "  --version        Print version and exit\n";

// The configuration of the local emitter, when there is no config file.
void SetDefaults(proto::Configuration* config) {
  const ui32 threads = std::max(1u, std::thread::hardware_concurrency());

  auto* emitter = config->mutable_emitter();
  emitter->set_socket_path(base::DefaultSocketPath());
  // The build system controls the parallelism - don't limit it.
  emitter->set_threads(threads * 2);

  auto* cache = config->mutable_cache();
  cache->set_path(base::DefaultCacheDir());
  cache->set_size(20ull << 30);
  cache->set_direct(true);
  cache->set_threads(threads * 2);
}

}  // namespace

Configuration::Configuration(int argc, char* argv[]) {
  String config_path, socket_path;

  for (int i = 1; i < argc; ++i) {
    const String arg = argv[i];
    if (arg == "--config" || arg == "-config") {
      if (++i == argc) {
        LOG(FATAL) << "Flag " << arg << " requires a value" << std::endl
                   << kUsage;
      }
      config_path = argv[i];
    } else if (arg.starts_with("--config=") || arg.starts_with("-config=")) {
      config_path = arg.substr(arg.find('=') + 1);
    } else if (arg == "--daemon" || arg == "-daemon") {
      daemonize_ = true;
    } else if (arg == "--autostart") {
      autostart_ = true;
      daemonize_ = true;
    } else if (arg.starts_with("--lock-fd=")) {
      lock_fd_ = std::stoi(arg.substr(strlen("--lock-fd=")));
    } else if (arg.starts_with("--socket=")) {
      socket_path = arg.substr(strlen("--socket="));
    } else if (arg == "--version") {
      std::cout << VERSION << std::endl;
      exit(0);
    } else if (arg == "--help" || arg == "-h") {
      std::cout << kUsage;
      exit(0);
    } else {
      LOG(FATAL) << "Unknown argument: " << arg << std::endl << kUsage;
    }
  }

  if (autostart_ && config_path.empty()) {
    config_path = base::DefaultDaemonConfigPath();
    if (!base::File::Exists(config_path)) {
      config_path.clear();
      SetDefaults(&config_);
    }
  }

  String error;
  if (!config_path.empty() &&
      !base::LoadFromFile(config_path, &config_, &error)) {
    LOG(ERROR) << "Failed to read configuration from " << config_path << ": "
               << error;
  }

  // The socket, that the client has locked: another one would be served by
  // another daemon.
  if (!socket_path.empty()) {
    config_.mutable_emitter()->set_socket_path(socket_path);
  }
}

}  // namespace daemon
}  // namespace dist_clang
