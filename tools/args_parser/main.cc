#include <base/base.pb.h>
#include <base/c_utils.h>
#include <base/file/file.h>
#include <base/logging.h>
#include <base/protobuf_utils.h>
#include <base/string_utils.h>
#include <client/command.hh>
#include <client/configuration.pb.h>

#include <clang/Basic/Version.h>

#include <base/using_log.h>

int main(int argc, const char* argv[]) {
  using namespace dist_clang;

  base::Log::Reset(10, {{INFO, FATAL}});

  Immutable version(true), clang_path(true);

  // Try to load config file first.
  String dir = base::GetCurrentDir();
  do {
    String config_path = dir + "/.distclang";
    client::proto::Configuration config;
    if (base::LoadFromFile(config_path, &config)) {
      clang_path = config.release_path();
      if (clang_path[0] != '/') {
        clang_path = Immutable(dir) + "/"_l + clang_path;
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

  client::Command::List commands;

  argv[0] = "clang";
  if (!client::Command::GenerateFromArgs(argc, argv, commands)) {
    LOG(FATAL) << "Failed to parse arguments";
  }

  String major_version;
  std::regex version_regex("clang version (\\d+\\.\\d+\\.\\d+)");
  std::cmatch match;
  if (std::regex_search(version.c_str(), match, version_regex) &&
      match.size() > 1 && match[1].matched) {
    LOG(VERBOSE) << "Matched Clang major version: " << match[1];
    major_version = match[1];
  } else {
    major_version = CLANG_VERSION_STRING;
  }

  for (const auto& command : commands) {
    LOG(INFO) << command->RenderAllArgs();

    base::proto::Flags flags;
    if (command->FillFlags(&flags, clang_path, major_version)) {
      LOG(INFO) << static_cast<const google::protobuf::Message&>(flags);
    }
  }

  return 0;
}
