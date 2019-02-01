#include <client/configuration.hh>

#include <base/constants.h>
#include <base/file/file.h>
#include <base/file_utils.h>
#include <base/logging.h>
#include <base/protobuf_utils.h>
#include <base/string_utils.h>
#include <base/types.h>

#include <base/using_log.h>

namespace dist_clang::client {

Configuration::Configuration() {
  // Try to load config file first.
  auto current_dir = base::GetCurrentDir();
  do {
    const auto config_path = current_dir / ".distclang";
    if (base::LoadFromFile(config_path, &config_)) {
      if (!Path(config_.path()).is_absolute()) {
        config_.set_path(current_dir / config_.path());
      }
      LOG(VERBOSE) << "Took compiler path from " << config_path << " : " << config_.path();

      if (config_.has_version()) {
        LOG(VERBOSE) << "Took version from " << config_path << " : " << config_.version();
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
          config_.mutable_plugins()->SwapElements(i--, config_.plugins_size() - 1);
          config_.mutable_plugins()->RemoveLast();
          continue;
        }

        if (!Path(plugin->path()).is_absolute()) {
          plugin->set_path(current_dir / plugin->path());
        }

        LOG(VERBOSE) << "Took plugin from " << config_path << " : " << plugin->name() << ", " << plugin->path();
      }

      break;
    }

    current_dir = current_dir.parent_path();
  } while (current_dir != current_dir.root_path());

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
    config_.mutable_verbosity()->set_error_mark(base::StringTo<ui32>(error_mark));
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

    String self_path;
    if (!base::GetSelfPath(self_path, nullptr)) {
      LOG(FATAL) << "Can't reliably detect myself in $PATH";
    }

    for (const auto& dir : path_dirs) {
      // TODO: convert |dir + "/clang"| to canonical path.
      if (base::File::IsExecutable(dir + "/clang") && dir != self_path) {
        config_.set_path(dir + "/clang");
        break;
      }
    }
  }

  CHECK(config_.read_timeout());
  CHECK(config_.send_timeout());
}

}  // namespace dist_clang::client
