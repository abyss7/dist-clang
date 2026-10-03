#include <client/clang.h>

#include <base/base.pb.h>
#include <base/file_utils.h>
#include <base/logging.h>
#include <client/configuration.pb.h>
#include <net/connection.h>
#include <net/end_point.h>
#include <net/network_service_impl.h>
#include <perf/stat.pb.h>

#include STL(iostream)

#include <unistd.h>

#include <base/using_log.h>

extern char** environ;

namespace dist_clang {
namespace client {

namespace {

net::ConnectionPtr Connect(const String& socket_path,
                           const proto::Configuration& config,
                           UniquePtr<net::NetworkService>* service) {
  *service = net::NetworkService::Create(
      config.connect_timeout(), config.read_timeout(), config.send_timeout(),
      config.read_minimum());

  String error;
  auto connection =
      (*service)->Connect(net::EndPoint::UnixSocket(socket_path), &error);
  if (!connection) {
    LOG(VERBOSE) << "Failed to connect to daemon: " << error;
  }
  return connection;
}

}  // namespace

Result DoMain(const List<String>& args, const String& socket_path,
              const proto::Configuration& config) {
  DCHECK(!args.empty());

  String error;
  auto current_dir = base::GetCurrentDir(&error);
  if (current_dir.empty()) {
    LOG(WARNING) << "Can't get current directory : " << error;
    return Result::RUN_LOCALLY;
  }

  UniquePtr<net::NetworkService> service;
  auto connection = Connect(socket_path, config, &service);
  if (!connection) {
    return Result::RUN_LOCALLY;
  }

  UniquePtr<base::proto::Local> message(new base::proto::Local);
  message->set_user_id(getuid());
  message->set_current_dir(current_dir);
  for (const auto& arg : args) {
    message->add_args(arg);
  }
  for (char** variable = environ; *variable; ++variable) {
    message->add_env(*variable);
  }

  net::proto::Status status;
  if (!connection->SendSync(std::move(message), &status)) {
    LOG(ERROR) << "Failed to send message: " << status.description();
    return Result::RUN_LOCALLY;
  }

  net::Connection::Message reply;
  if (!connection->ReadSync(&reply, &status)) {
    LOG(ERROR) << "Failed to read message: " << status.description();
    return Result::RUN_LOCALLY;
  }

  if (!reply.HasExtension(net::proto::Status::extension)) {
    LOG(ERROR) << "Message from daemon has wrong format : no Status extension";
    return Result::RUN_LOCALLY;
  }

  status = reply.GetExtension(net::proto::Status::extension);
  switch (status.code()) {
    case net::proto::Status::OK:
      std::cerr << status.description();
      return Result::SUCCESS;
    case net::proto::Status::EXECUTION:
      std::cerr << status.description();
      return Result::FAILURE;
    default:
      LOG(VERBOSE) << "Daemon didn't handle the command: "
                   << status.description();
      return Result::RUN_LOCALLY;
  }
}

bool PrintStats(const String& socket_path,
                const proto::Configuration& config) {
  UniquePtr<net::NetworkService> service;
  auto connection = Connect(socket_path, config, &service);
  if (!connection) {
    std::cerr << "Daemon isn't running on " << socket_path << std::endl;
    return false;
  }

  net::proto::Status status;
  if (!connection->SendSync(std::make_unique<perf::proto::Report>(),
                            &status)) {
    std::cerr << "Failed to send request: " << status.description()
              << std::endl;
    return false;
  }

  net::Connection::Message reply;
  if (!connection->ReadSync(&reply, &status) ||
      !reply.HasExtension(perf::proto::Report::extension)) {
    std::cerr << "Failed to read statistics: " << status.description()
              << std::endl;
    return false;
  }

  for (const auto& metric :
       reply.GetExtension(perf::proto::Report::extension).metric()) {
    std::cout << perf::proto::Metric::Name_Name(metric.name()) << " "
              << metric.value() << std::endl;
  }

  return true;
}

}  // namespace client
}  // namespace dist_clang
