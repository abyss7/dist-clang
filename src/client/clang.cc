#include <client/clang.h>

#include <base/assert.h>
#include <base/c_utils.h>
#include <base/constants.h>
#include <base/file_utils.h>
#include <base/logging.h>
#include <base/process_impl.h>
#include <base/string_utils.h>
#include <client/clang_flag_set.h>
#include <net/base/end_point.h>
#include <net/connection.h>
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
  String error;
  auto connection = service->Connect(end_point, &error);
  if (!connection) {
    LOG(WARNING) << "Failed to connect to daemon: " << error;
    return true;
  }

  String current_dir = base::GetCurrentDir();
  if (current_dir.empty()) {
    return true;
  }

  String version;
  FlagSet::CommandList commands;
  base::ProcessPtr process =
      base::Process::Create(clang_path, String(), base::Process::SAME_UID);
  process->AppendArg("-###").AppendArg(argv + 1, argv + argc);
  if (!process->Run(10) ||
      !FlagSet::ParseClangOutput(process->stderr(), &version, commands)) {
    return true;
  }

  for (auto& args : commands) {
    UniquePtr<proto::Execute> message(new proto::Execute);
    message->set_remote(false);
    message->set_user_id(getuid());
    message->set_current_dir(current_dir);

    auto flags = message->mutable_flags();
    flags->mutable_compiler()->set_version(version);
    if (FlagSet::ProcessFlags(args, flags) != FlagSet::COMPILE) {
      base::ProcessPtr command = base::Process::Create(args.front(), String(),
                                                       base::Process::SAME_UID);
      command->AppendArg(++args.begin(), args.end());
      if (!command->Run(base::Process::UNLIMITED)) {
        return true;
      }

      continue;
    }

#ifndef NDEBUG
    // FIXME: refactor this in a better way.
    String object_file = flags->output(), deps_file;
    bool next = false;
    for (const auto& flag : flags->dependenies()) {
      if (flag == "-dependency-file") {
        next = true;
      } else if (next) {
        deps_file = flag;
        break;
      }
    }
#endif

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

    DCHECK(base::FileExists(object_file));
    DCHECK(base::FileExists(deps_file));

    LOG(VERBOSE) << "Compilation on daemon successful" << std::endl;
  }

  return false;
}

}  // namespace client
}  // namespace dist_clang
