#include <client/configuration.hh>

#include <base/aliases.h>
#include <base/c_utils.h>
#include <base/constants.h>
#include <base/file/file.h>
#include <base/logging.h>
#include <base/protobuf_utils.h>
#include <base/string_utils.h>

#include <base/using_log.h>

namespace dist_clang {
namespace client {

Configuration::Configuration() {
  // Try to load config file first.
  String current_dir = base::GetCurrentDir();
  do {
    String config_path = current_dir + "/.distclang";
    if (base::LoadFromFile(config_path, &config_)) {
      if (config_.path()[0] != '/') {
        config_.set_path(current_dir + "/" + config_.path());
      }
      LOG(VERBOSE) << "Took compiler path from " << config_path << " : "
                   << config_.path();

      if (config_.has_version()) {
        LOG(VERBOSE) << "Took version from " << config_path << " : "
                     << config_.version();
      }

      for (int i = 0; i < config_.plugins_size(); ++i) {
        auto* plugin = config_.mutable_plugins(i);
#if defined(OS_LINUX)
        auto os = client::proto::Plugin::LINUX;
#elif defined(OS_MACOSX)
        auto os = client::proto::Plugin::MACOSX;
#elif defined(OS_WIN)
        auto os = client::proto::Plugin::WIN;
#else
        auto os = client::proto::Plugin::UNKNOWN;
#endif
        if (plugin->os() != os) {
          config_.mutable_plugins()->SwapElements(i--,
                                                  config_.plugins_size() - 1);
          config_.mutable_plugins()->RemoveLast();
          continue;
        }

        if (plugin->path()[0] != '/') {
          plugin->set_path(current_dir + "/" + plugin->path());
        }

        LOG(VERBOSE) << "Took plugin from " << config_path << " : "
                     << plugin->name() << ", " << plugin->path();
      }

      break;
    }

    current_dir = current_dir.substr(0, current_dir.find_last_of("/"));
  } while (!current_dir.empty());

  // Environment variables prevail over config file.
  Immutable log_levels = base::GetEnv(base::kEnvLogLevels);
  if (!log_levels.empty()) {
    List<String> numbers;

    base::SplitString<' '>(log_levels, numbers);
    if (numbers.size() % 2 == 0) {
      config_.mutable_verbosity()->clear_levels();
      for (auto number = numbers.begin(); number != numbers.end(); ++number) {
        auto* range = config_.mutable_verbosity()->add_levels();
        range->set_left(base::StringTo<ui32>(*number++));
        range->set_right(base::StringTo<ui32>(*number));
      }
    }
  }

  Immutable error_mark = base::GetEnv(base::kEnvLogErrorMark);
  if (!error_mark.empty()) {
    config_.mutable_verbosity()->set_error_mark(
        base::StringTo<ui32>(error_mark));
  }

  Immutable version = base::GetEnv(base::kEnvClangVersion);
  if (!version.empty()) {
    config_.set_version(version);
  }

  Immutable clang_path = base::GetEnv(base::kEnvClangPath);
  if (!clang_path.empty()) {
    config_.set_path(clang_path);
  }

  Immutable disabled = base::GetEnv(base::kEnvDisabled);
  if (!disabled.empty()) {
    config_.set_disabled(true);
  }

  if (config_.path().empty()) {
    Immutable path = base::GetEnv("PATH"_l);
    List<String> path_dirs;
    base::SplitString<':'>(path, path_dirs);

    for (const auto& dir : path_dirs) {
      // TODO: convert |dir + "/clang"| to canonical path.
      if (base::File::IsExecutable(dir + "/clang") &&
          dir != base::GetSelfPath()) {
        config_.set_path(dir + "/clang");
        break;
      }
    }
  }

  CHECK(config_.read_timeout());
  CHECK(config_.send_timeout());
}

}  // namespace client
}  // namespace dist_clang
