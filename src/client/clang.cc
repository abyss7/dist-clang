#include "client/clang.h"

#include "base/assert.h"
#include "base/c_utils.h"
#include "base/constants.h"
#include "base/logging.h"
#include "base/process_impl.h"
#include "base/string_utils.h"
#include "client/clang_flag_set.h"
#include "net/base/end_point.h"
#include "net/connection.h"
#include "net/network_service_impl.h"
#include "proto/remote.pb.h"

#include <list>

#include "base/using_log.h"

using namespace dist_clang;

namespace {



}  // namespace

namespace dist_clang {
namespace client {

bool DoMain(int argc, const char* const argv[], const std::string& socket_path,
            const std::string& clang_path) {
  if (clang_path.empty()) {
    return true;
  }

  auto service = net::NetworkService::Create();
  auto end_point = net::EndPoint::UnixSocket(socket_path);
  std::string error;
  auto connection = service->Connect(end_point, &error);
  if (!connection) {
    LOG(WARNING) << "Failed to connect to daemon: " << error;
    return true;
  }

  std::unique_ptr<proto::Execute> message(new proto::Execute);
  message->set_remote(false);

  std::string current_dir = base::GetCurrentDir();
  if (current_dir.empty()) {
    return true;
  }
  message->set_current_dir(current_dir);

  {
    auto flags = message->mutable_cc_flags();
    auto version = flags->mutable_compiler()->mutable_version();
    ClangFlagSet::StringList args;
    base::ProcessPtr process = base::Process::Create(clang_path, std::string());
    process->AppendArg("-###").AppendArg(argv + 1, argv + argc);
    if (!process->Run(10) ||
        !ClangFlagSet::ParseClangOutput(process->stderr(), version, args) ||
         ClangFlagSet::ProcessFlags(args, flags) != ClangFlagSet::COMPILE) {
      return true;
    }
  }

  {
    auto flags = message->mutable_pp_flags();
    auto version = flags->mutable_compiler()->mutable_version();
    ClangFlagSet::StringList args;
    base::ProcessPtr process = base::Process::Create(clang_path, std::string());
    process->AppendArg("-###").AppendArg("-E").AppendArg("-P")
            .AppendArg(argv + 1, argv + argc);
    if (!process->Run(10) ||
        !ClangFlagSet::ParseClangOutput(process->stderr(), version, args) ||
         ClangFlagSet::ProcessFlags(args, flags) != ClangFlagSet::PREPROCESS) {
      return true;
    }
    flags->clear_output();
  }

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

  return false;
}

}  // namespace client
}  // namespace dist_clang
