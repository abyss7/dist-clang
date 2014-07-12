#include <daemon/configuration.h>

#include <base/constants.h>
#include <base/logging.h>
#include <base/string_utils.h>

#include <third_party/protobuf/src/google/protobuf/io/zero_copy_stream_impl.h>
#include <third_party/protobuf/src/google/protobuf/text_format.h>
#include <third_party/tclap/exported/include/tclap/CmdLine.h>

#include <fcntl.h>

#include <base/using_log.h>

using namespace TCLAP;

namespace dist_clang {
namespace daemon {

Configuration::Configuration(int argc, char *argv[]) {
  try {
    CmdLine cmd("Daemon from Clang distributed system - Clangd.", ' ', VERSION);
    ValueArg<String> socket_arg(
        "s", "socket", "Path to UNIX socket to listen for local connections.",
        false, base::kDefaultSocketPath, "path", cmd);
    ValueArg<String> cache_arg(
        "c", "cache", "Path, where the daemon will cache compilation results.",
        false, String(), "path", cmd);
    MultiArg<String> hosts_arg("", "host",
                               "Remote host to use for remote compilation.",
                               false, "host[:port[:cpus]]", cmd);
    ValueArg<String> local_arg("l", "listen",
                               "Local host to listen for remote connections.",
                               false, String(), "host[:port[:cpus]]", cmd);
    ValueArg<String> config_arg("", "config", "Path to the configuration file.",
                                false, String(), "path", cmd);
    cmd.parse(argc, argv);

    // Configuration file, if provided, has lower priority then command-line
    // arguments.
    if (config_arg.isSet() && !LoadFromFile(config_arg.getValue())) {
      LOG(ERROR) << "Failed to read config file: " << config_arg.getValue();
    }

    if (socket_arg.isSet()) {
      config_.set_socket_path(socket_arg.getValue());
    }

    if (cache_arg.isSet()) {
      config_.mutable_cache()->set_path(cache_arg.getValue());
    }

    for (auto host : hosts_arg) {
      List<String> strs;
      base::SplitString<':'>(host, strs);
      if (strs.size() <= 3) {
        auto remote = config_.add_remotes();
        remote->set_host(strs.front());
        strs.pop_front();
        if (!strs.empty()) {
          remote->set_port(base::StringTo<ui32>(strs.front()));
          strs.pop_front();
        }
        if (!strs.empty()) {
          remote->set_threads(base::StringTo<ui32>(strs.front()));
          strs.pop_front();
        }
      } else {
        strs.clear();
      }
    }

    if (local_arg.isSet()) {
      List<String> strs;
      base::SplitString<':'>(local_arg.getValue(), strs);
      if (strs.size() <= 3) {
        auto local = config_.mutable_local();
        local->set_host(strs.front());
        strs.pop_front();
        if (!strs.empty()) {
          local->set_port(base::StringTo<ui32>(strs.front()));
          strs.pop_front();
        }
        if (!strs.empty()) {
          local->set_threads(base::StringTo<ui32>(strs.front()));
          strs.pop_front();
        }
      }
    }
  }
  catch (ArgException &e) {
    LOG(ERROR) << e.argId() << std::endl << e.error();
    return;
  }
}

Configuration::Configuration(const proto::Configuration &config)
    : config_(config) {}

bool Configuration::LoadFromFile(const String &config_path) {
  auto fd = open(config_path.c_str(), O_RDONLY);
  if (fd == -1) {
    return false;
  }
  google::protobuf::io::FileInputStream input(fd);
  input.SetCloseOnDelete(true);
  if (!google::protobuf::TextFormat::Parse(&input, &config_)) {
    config_.Clear();
    return false;
  }
  return true;
}

const proto::Configuration &Configuration::config() const { return config_; }

}  // namespace daemon
}  // namespace dist_clang
