#include <client/clang.h>

#include <base/base.pb.h>
#include <base/c_utils.h>
#include <base/logging.h>
#include <base/process_impl.h>
#include <base/string_utils.h>
#include <client/command.h>
#include <net/connection.h>
#include <net/end_point.h>
#include <net/network_service_impl.h>

#include <clang/Basic/Version.h>

#include <base/using_log.h>

namespace dist_clang {
namespace client {

bool DoMain(int argc, const char* const argv[], Immutable socket_path,
            Immutable clang_path, Immutable version, ui32 read_timeout_secs,
            ui32 send_timeout_secs, ui32 read_min_bytes,
            const HashMap<String, String>& plugins) {
  if (clang_path.empty()) {
    return true;
  }

  auto service = net::NetworkService::Create(read_timeout_secs,
                                             send_timeout_secs, read_min_bytes);
  auto end_point = net::EndPoint::UnixSocket(socket_path);

  String error;
  auto current_dir = base::GetCurrentDir(&error);
  if (current_dir.empty()) {
    LOG(WARNING) << "Can't get current directory : " << error;
    return true;
  }

  Command::List commands;

  if (!DriverCommand::GenerateFromArgs(argc, argv, commands)) {
    LOG(WARNING) << "Failed to parse driver arguments - see errors above";
    return true;
  }

  auto connection = service->Connect(end_point, &error);
  if (!connection) {
    LOG(WARNING) << "Failed to connect to daemon: " << error;
    return true;
  }

  for (const auto& command : commands) {
    if (!command->IsClang()) {
      auto process = command->CreateProcess(current_dir, getuid());
      if (!process->Run(base::Process::UNLIMITED)) {
        LOG(WARNING) << "Subcommand failed: " << command->GetExecutable()
                     << std::endl
                     << process->stderr();
        LOG(VERBOSE) << "Arguments: " << command->RenderAllArgs();
        return true;
      }
      continue;
    }

    UniquePtr<base::proto::Local> message(new base::proto::Local);
    message->set_user_id(getuid());
    message->set_current_dir(current_dir);

    if (version.empty()) {
      String error;
      auto process =
          base::Process::Create(clang_path, String(), base::Process::SAME_UID);
      process->AppendArg("--version"_l);
      if (!process->Run(1, &error)) {
        if (error.empty()) {
          error = process->stderr();
        } else if (!process->stderr().empty()) {
          error += Immutable(", "_l) + process->stderr();
        }

        if (!error.empty()) {
          LOG(WARNING) << "Can't get compiler version : " << error;
        } else {
          LOG(WARNING) << "Can't get compiler version by unknown reason";
        }

        return true;
      }

      List<String> output;
      base::SplitString<'\n'>(process->stdout(), output);
      if (output.size() != 3) {
        if (!process->stderr().empty()) {
          LOG(ERROR) << process->stderr();
        }
        LOG(WARNING) << "Clang binary has unknown version format : "
                     << process->stdout();
        return true;
      }

      version = Immutable(*output.begin());
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

    auto* flags = message->mutable_flags();
    command->AsDriverCommand()->FillFlags(flags, clang_path, major_version);

    if (!flags->has_action() || flags->input() == "-") {
      LOG(WARNING) << "Command line contains unknown action or requires input "
                      "from stdin";
      return true;
    }

    flags->mutable_compiler()->set_version(version);
    flags->mutable_compiler()->set_path(clang_path);

    auto& mutable_plugins = *flags->mutable_compiler()->mutable_plugins();
    for (auto& plugin : mutable_plugins) {
      auto it = plugins.find(plugin.name());
      if (it != plugins.end()) {
        plugin.set_path(it->second);
      }
    }

    net::proto::Status status;
    if (!connection->SendSync(std::move(message), &status)) {
      LOG(ERROR) << "Failed to send message: " << status.description();
      return true;
    }

    net::Connection::Message top_message;
    if (!connection->ReadSync(&top_message, &status)) {
      LOG(ERROR) << "Failed to read message: " << status.description();
      return true;
    }

    if (!top_message.HasExtension(net::proto::Status::extension)) {
      LOG(ERROR)
          << "Message from daemon has wrong format : no Status extension";
      return true;
    }

    status = top_message.GetExtension(net::proto::Status::extension);
    if (status.code() != net::proto::Status::EXECUTION &&
        status.code() != net::proto::Status::OK) {
      LOG(VERBOSE) << "Failed to use daemon: " << status.description();
      return true;
    }

    if (status.code() == net::proto::Status::EXECUTION) {
      LOG(FATAL) << "Compilation on daemon failed:" << std::endl
                 << status.description();
    }

    LOG(VERBOSE) << "Compilation on daemon successful" << std::endl;
  }

  return false;
}

}  // namespace client
}  // namespace dist_clang
