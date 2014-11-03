#include <client/clang.h>

#include <base/c_utils.h>
#include <base/logging.h>
#include <base/process_impl.h>
#include <base/string_utils.h>
#include <client/command.h>
#include <net/connection.h>
#include <net/end_point.h>
#include <net/network_service_impl.h>
#include <proto/remote.pb.h>

#include <base/using_log.h>

namespace dist_clang {
namespace client {

bool DoMain(int argc, const char* const argv[], const String& socket_path,
            const String& clang_path, String version) {
  if (clang_path.empty()) {
    return true;
  }

  auto service = net::NetworkService::Create();
  auto end_point = net::EndPoint::UnixSocket(socket_path);

  String current_dir = base::GetCurrentDir();
  if (current_dir.empty()) {
    return true;
  }

  Command::List commands;

  if (!DriverCommand::GenerateFromArgs(argc, argv, commands)) {
    LOG(WARNING) << "Failed to parse driver arguments - see errors above";
    return true;
  }

  String error;
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
                     << std::endl << process->stderr();
        LOG(VERBOSE) << "Arguments: " << command->RenderAllArgs();
        return true;
      }
      continue;
    }

    UniquePtr<proto::LocalExecute> message(new proto::LocalExecute);
    message->set_user_id(getuid());
    message->set_current_dir(current_dir);

    auto* flags = message->mutable_flags();
    command->AsDriverCommand()->FillFlags(flags, clang_path);

    if (!flags->has_action() || flags->input() == "-") {
      return true;
    }

    if (version.empty()) {
      auto process =
          base::Process::Create(clang_path, String(), base::Process::SAME_UID);
      process->AppendArg("--version");
      if (!process->Run(1)) {
        return true;
      }

      List<String> output;
      base::SplitString<'\n'>(process->stdout(), output);
      if (output.size() != 3) {
        return true;
      }

      version = *output.begin();
    }
    flags->mutable_compiler()->set_version(version);
    flags->mutable_compiler()->set_path(clang_path);

    proto::Status status;
    if (!connection->SendSync(std::move(message), &status)) {
      LOG(ERROR) << "Failed to send message: " << status.description();
      return true;
    }

    net::Connection::Message top_message;
    if (!connection->ReadSync(&top_message, &status)) {
      LOG(ERROR) << "Failed to read message: " << status.description();
      return true;
    }

    if (!top_message.HasExtension(proto::Status::extension)) {
      return true;
    }

    status = top_message.GetExtension(proto::Status::extension);
    if (status.code() != proto::Status::EXECUTION &&
        status.code() != proto::Status::OK) {
      LOG(VERBOSE) << "Failed to use daemon: " << status.description();
      return true;
    }

    if (status.code() == proto::Status::EXECUTION) {
      LOG(FATAL) << "Compilation on daemon failed:" << std::endl
                 << status.description();
    }

    LOG(VERBOSE) << "Compilation on daemon successful" << std::endl;
  }

  return false;
}

}  // namespace client
}  // namespace dist_clang
