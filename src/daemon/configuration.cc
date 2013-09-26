#include "daemon/configuration.h"

#include "base/constants.h"
#include "base/string_utils.h"
#include "tclap/CmdLine.h"

#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>
#include <string>

#include <fcntl.h>

using std::string;
using namespace TCLAP;

namespace dist_clang {
namespace daemon {

Configuration::Configuration(int argc, char *argv[]) {
  try {
    // TODO: use normal versioning.
    CmdLine cmd("Daemon from Clang distributed system - Clangd.", ' ', "0.1");
    ValueArg<string> socket_arg("s", "socket",
        "Path to UNIX socket to listen for local connections.",
        false, base::kDefaultClangdSocket, "path", cmd);
    ValueArg<string> cache_arg("c", "cache",
        "Path, where the daemon will cache compilation results.",
        false, string(), "path", cmd);
    MultiArg<string> hosts_arg("", "host",
        "Remote host to use for remote compilation.",
        false, "host[:port[:cpus]]", cmd);
    ValueArg<string> local_arg("l", "listen",
        "Local host to listen for remote connections.",
        false, string(), "host[:port[:cpus]]", cmd);
    ValueArg<string> config_arg("", "config",
        "Path to the configuration file.",
        false, string(), "path", cmd);
    cmd.parse(argc, argv);

    // Configuration file, if provided, has lower priority then command-line
    // arguments.
    if (config_arg.isSet())
      LoadFromFile(config_arg.getValue());

    if (!config_.has_socket_path())
      config_.set_socket_path(socket_arg.getValue());

    if (cache_arg.isSet())
      config_.set_cache_path(cache_arg.getValue());

    for (auto host: hosts_arg) {
      std::list<string> strs;
      base::SplitString<':'>(host, strs);
      if (strs.size() <= 3) {
        auto remote = config_.add_remotes();
        remote->set_host(strs.front());
        strs.pop_front();
        if (!strs.empty()) {
          remote->set_port(base::StringToInt<unsigned short>(strs.front()));
          strs.pop_front();
        }
        if (!strs.empty()) {
          remote->set_threads(base::StringToInt<unsigned>(strs.front()));
          strs.pop_front();
        }
      } else {
        strs.clear();
      }
    }

    if (local_arg.isSet()) {
      std::list<string> strs;
      base::SplitString<':'>(local_arg.getValue(), strs);
      if (strs.size() <= 3) {
        auto local = config_.mutable_local();
        local->set_host(strs.front());
        strs.pop_front();
        if (!strs.empty()) {
          local->set_port(base::StringToInt<unsigned short>(strs.front()));
          strs.pop_front();
        }
        if (!strs.empty()) {
          local->set_threads(base::StringToInt<unsigned>(strs.front()));
          strs.pop_front();
        }
      }
    }
  } catch (ArgException &e) {
    std::cerr << e.argId() << std::endl << e.error() << std::endl;
    return;
  }
}

bool Configuration::LoadFromFile(const std::string &config_path) {
  auto fd = open(config_path.c_str(), O_RDONLY);
  if (fd == -1)
    return false;
  google::protobuf::io::FileInputStream input(fd);
  input.SetCloseOnDelete(true);
  if (!google::protobuf::TextFormat::Parse(&input, &config_)) {
    config_.Clear();
    return false;
  }
  return true;
}

const proto::Configuration& Configuration::config() const {
  return config_;
}

}  // namespace daemon
}  // namespace dist_clang
