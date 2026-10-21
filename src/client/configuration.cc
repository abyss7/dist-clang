#include <client/configuration.hh>

#include <base/constants.h>
#include <base/file/file.h>
#include <base/file_utils.h>
#include <base/logging.h>
#include <base/platform.h>
#include <base/protobuf_utils.h>
#include <base/string_utils.h>
#include <base/types.h>

#include <base/using_log.h>

namespace dist_clang::client {

Configuration::Configuration() {
  // Try to load config file first - from the current directory up to the root.
  for (auto current_dir = base::GetCurrentDir();;
       current_dir = current_dir.parent_path()) {
    const auto config_path = current_dir / ".distclang";
    if (base::LoadFromFile(config_path, &config_)) {
      if (config_.has_path()) {
        if (!Path(config_.path()).is_absolute()) {
          config_.set_path(current_dir / config_.path());
        }
        LOG(VERBOSE) << "Took compiler path from " << config_path << " : "
                     << config_.path();
      }

      const String unsupported = UnsupportedOptions(config_);
      if (!unsupported.empty()) {
        warnings_.push_back(config_path.string() +
                            ": the options aren't supported anymore and are "
                            "ignored: " +
                            unsupported);
      }

      break;
    }

    if (current_dir == current_dir.root_path() || current_dir.empty()) {
      break;
    }
  }

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

  Immutable clang_path = base::GetEnv(base::kEnvClangPath);
  if (!clang_path.empty()) {
    config_.set_path(clang_path);
  }

  Immutable disabled = base::GetEnv(base::kEnvDisabled);
  if (!disabled.empty()) {
    config_.set_disabled(true);
  }

  // The real compiler mustn't be the client itself - e.g. its symlink: it
  // would run itself forever.
  String self;
  if (!base::GetSelfExecutable(self, nullptr)) {
    LOG(FATAL) << "Can't reliably detect myself in $PATH";
  }
  std::error_code ec;
  const auto self_path = std::filesystem::canonical(self, ec);
  auto IsSelf = [&](const String& path) {
    std::error_code ec;
    const auto real_path = std::filesystem::canonical(path, ec);
    return !ec && real_path == self_path;
  };

  if (!config_.path().empty() && IsSelf(config_.path())) {
    LOG(ERROR) << "The real compiler " << config_.path()
               << " is dist-clang itself";
    config_.clear_path();
  }

  if (config_.path().empty()) {
    Immutable path = base::GetEnv("PATH"_l);
    List<String> path_dirs;
    base::SplitString<':'>(path, path_dirs);

    for (const auto& dir : path_dirs) {
      const String candidate = dir + "/clang";
      if (base::File::IsExecutable(candidate) && !IsSelf(candidate)) {
        config_.set_path(candidate);
        break;
      }
    }
  }

  CHECK(config_.read_timeout());
  CHECK(config_.send_timeout());
}

String UnsupportedOptions(const proto::Configuration& config) {
  String options;
  for (const auto& [option, used] :
       {Pair<const char*, bool>{"plugins", config.plugins_size() > 0},
        {"version", config.has_version()},
        {"rewrite_includes", config.rewrite_includes()}}) {
    if (used) {
      options += (options.empty() ? "" : ", ") + String(option);
    }
  }
  return options;
}

}  // namespace dist_clang::client
