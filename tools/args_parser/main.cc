#include <base/logging.h>
#include <base/protobuf_utils.h>  // for "LOG() << google::protobuf::Message"
#include <client/command.hh>
#include <client/configuration.hh>

#include <clang/Basic/Version.h>

#include <base/using_log.h>

int main(int argc, const char* argv[]) {
  using namespace dist_clang;

  client::Configuration configuration;
  const auto& config = configuration.config();

  base::Log::Reset(ERROR, {{INFO, FATAL}});

  client::Command::List commands;

  argv[0] = "clang";
  if (!client::Command::GenerateFromArgs(argc, argv, commands)) {
    LOG(FATAL) << "Failed to parse arguments";
  }

  String major_version;
  std::regex version_regex("clang version (\\d+\\.\\d+\\.\\d+)");
  std::cmatch match;
  if (std::regex_search(config.version().c_str(), match, version_regex) &&
      match.size() > 1 && match[1].matched) {
    LOG(VERBOSE) << "Matched Clang major version: " << match[1];
    major_version = match[1];
  } else {
    major_version = CLANG_VERSION_STRING;
  }

  for (const auto& command : commands) {
    LOG(INFO) << command->RenderAllArgs();

    base::proto::Flags flags;
    if (command->FillFlags(&flags, config.path(), major_version)) {
      LOG(INFO) << static_cast<const google::protobuf::Message&>(flags);
    }
  }

  return 0;
}
