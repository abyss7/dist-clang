#include <base/assert.h>
#include <base/c_utils.h>
#include <base/constants.h>
#include <base/file/file.h>
#include <base/logging.h>
#include <base/string_utils.h>
#include <client/clang.h>
#include <client/configuration.pb.h>

#include <third_party/protobuf/exported/src/google/protobuf/io/zero_copy_stream_impl.h>
#include <third_party/protobuf/exported/src/google/protobuf/text_format.h>

#include <fcntl.h>
#include <signal.h>

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

  // It's safe to use |base::Log|, since its internal static objects don't need
  // special destruction on |exec|.
  Immutable clangd_log_levels = base::GetEnv(base::kEnvLogLevels);

  if (!clangd_log_levels.empty()) {
    Immutable clangd_log_mark = base::GetEnv(base::kEnvLogErrorMark);
    List<String> numbers;

    base::SplitString<' '>(clangd_log_levels, numbers);
    if (numbers.size() % 2 == 0) {
      base::Log::RangeSet ranges;
      for (auto number = numbers.begin(); number != numbers.end(); ++number) {
        ui32 left = base::StringTo<ui32>(*number++);
        ui32 right = base::StringTo<ui32>(*number);
        ranges.emplace(right, left);
      }
      base::Log::Reset(base::StringTo<ui32>(clangd_log_mark),
                       std::move(ranges));
    }
  }

  Immutable version, clang_path;

  // Try to load config file first.
  String dir = base::GetCurrentDir();
  do {
    String config_path = dir + "/.distclang";
    if (base::File::Exists(config_path)) {
      client::proto::Configuration config;

      auto fd = open(config_path.c_str(), O_RDONLY);
      if (fd == -1) {
        break;
      }

      google::protobuf::io::FileInputStream input(fd);
      input.SetCloseOnDelete(true);
      if (!google::protobuf::TextFormat::Parse(&input, &config)) {
        LOG(INFO) << "Found " << config_path << " but it's broken!";
        break;
      }

      clang_path = config.release_path();
      if (clang_path[0] != '/') {
        clang_path = Immutable(std::move(dir)) + "/"_l + clang_path;
      }
      LOG(VERBOSE) << "Took compiler path from " << config_path << " : "
                   << clang_path;

      if (config.has_version()) {
        version = config.release_version();
        LOG(VERBOSE) << "Took version from " << config_path << " : " << version;
      }

      break;
    }

    dir = dir.substr(0, dir.find_last_of("/"));
  } while (!dir.empty());

  // FIXME: Make default socket path the build param - for consistent packaging.
  Immutable socket_path =
      base::GetEnv(base::kEnvSocketPath, base::kDefaultSocketPath);

  Immutable version_env = base::GetEnv(base::kEnvClangVersion);
  Immutable clang_path_env = base::GetEnv(base::kEnvClangPath);
  if (!version_env.empty()) {
    version = version_env;
  }
  if (!clang_path_env.empty()) {
    clang_path = clang_path_env;
  }

  if (clang_path.empty()) {
    Immutable path = base::GetEnv("PATH"_l);
    List<String> path_dirs;
    base::SplitString<':'>(path, path_dirs);

    for (const auto& dir : path_dirs) {
      // TODO: convert |dir + "/clang"| to canonical path.
      if (base::File::IsExecutable(dir + "/clang") &&
          dir != base::GetSelfPath()) {
        clang_path = Immutable(dir + "/clang");
        break;
      }
    }
  }

  // NOTICE: Use separate |DoMain| function to make sure that all local objects
  //         get destructed before the invokation of |exec|. Do not use global
  //         objects!
  if (client::DoMain(argc, argv, socket_path, clang_path, version)) {
    return ExecuteLocally(argv, clang_path);
  }

  return 0;
}
