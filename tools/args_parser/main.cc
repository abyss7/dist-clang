#include <base/file_utils.h>
#include <base/logging.h>
#include <base/process_impl.h>
#include <base/protobuf_utils.h>  // for "LOG() << google::protobuf::Message"
#include <base/test_process.h>
#include <daemon/compilation_daemon.h>
#include <client/command.hh>
#include <client/configuration.hh>

#include <clang/Basic/Version.h>

#include <base/using_log.h>

int main(int argc, const char* argv[]) {
  using namespace dist_clang;

  client::Configuration configuration;
  const auto& config = configuration.config();

  base::Log::Reset(ERROR, {{VERBOSE, FATAL}});

  client::Command::List commands;

  argv[0] = "clang";
  if (!client::Command::GenerateFromArgs(argc, argv, commands)) {
    LOG(FATAL) << "Failed to parse arguments";
  }

  String major_version;
  std::regex version_regex("clang version (\\d+\\.\\d+\\.\\d+)");
  std::cmatch match;
  if (std::regex_search(config.version().c_str(), match, version_regex) && match.size() > 1 && match[1].matched) {
    LOG(VERBOSE) << "Matched Clang major version: " << match[1];
    major_version = match[1];
  } else {
    major_version = CLANG_VERSION_STRING;
  }

  for (const auto& command : commands) {
    auto&& log = LOG(INFO);
    auto factory = base::Process::SetFactory<base::TestProcess::Factory>();
    factory->CallOnCreate([&log](base::TestProcess* process) {
      process->CallOnRun([&log, process](ui32, const String&, String*) {
        log << process->PrintArgs();
        return true;
      });
    });

    log << command->RenderAllArgs() << std::endl;

    base::proto::Flags flags;
    if (command->FillFlags(&flags, config.path(), major_version, false)) {
      log << static_cast<const google::protobuf::Message&>(flags);
    }

    log << std::endl;

    auto process = daemon::CompilationDaemon::CreateProcess(flags, base::GetCurrentDir());
    process->Run(0);

    log << std::endl;
  }

  return 0;
}
