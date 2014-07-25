#include <client/clang.h>

#include <base/c_utils.h>
#include <base/constants.h>
#include <base/logging.h>
#include <client/command.h>
#include <net/base/end_point.h>
#include <net/network_service_impl.h>
#include <proto/remote.pb.h>

#include <base/using_log.h>

namespace dist_clang {
namespace client {

bool DoMain(int argc, const char* const argv[], const String& socket_path,
            const String& clang_path) {
  if (clang_path.empty()) {
    return true;
  }

  auto service = net::NetworkService::Create();
  auto end_point = net::EndPoint::UnixSocket(socket_path);

  String current_dir = base::GetCurrentDir();
  if (current_dir.empty()) {
    return true;
  }

  String version = base::GetEnv(base::kEnvClangVersion);
  List<Command> commands;

  if (!Command::GenerateFromArgs(argc, argv, commands)) {
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
    if (!command.IsClang()) {
      auto process = command.CreateProcess(current_dir, getuid());
      if (!process->Run(base::Process::UNLIMITED)) {
        LOG(WARNING) << "Subcommand failed: " << command.GetExecutable()
                     << std::endl << process->stderr();
        LOG(VERBOSE) << "Arguments: " << command.RenderAllArgs();
        return true;
      }
      continue;
    }

    UniquePtr<proto::Execute> message(new proto::Execute);
    message->set_remote(false);
    message->set_user_id(getuid());
    message->set_current_dir(current_dir);

    auto* flags = message->mutable_flags();
    command.FillFlags(flags, clang_path);

    if (!flags->has_action() || flags->input() == "-") {
      return true;
    }

    if (version.empty()) {
      // TODO: extract version from |clang_path|.
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
