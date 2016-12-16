#include <daemon/coordinator.h>

#include <base/assert.h>
#include <base/logging.h>
#include <net/connection_impl.h>

#include <base/using_log.h>

namespace dist_clang {
namespace daemon {

Coordinator::Coordinator(const proto::Configuration& configuration)
    : BaseDaemon(configuration) {
  CHECK(configuration.has_coordinator());
}

bool Coordinator::Initialize() {
  String error;
  const auto& local = conf()->coordinator().local();
  if (!Listen(local.host(), local.port(), local.ipv6(), &error)) {
    LOG(ERROR) << "[Coordinator] Failed to listen on " << local.host() << ":"
               << local.port() << " : " << error;
    return false;
  }

  return BaseDaemon::Initialize();
}

bool Coordinator::HandleNewMessage(net::ConnectionPtr connection,
                                   Universal message,
                                   const net::proto::Status& status) {
  if (!message->IsInitialized()) {
    LOG(INFO) << message->InitializationErrorString();
    return false;
  }

  if (status.code() != net::proto::Status::OK) {
    LOG(ERROR) << status.description();
    return connection->ReportStatus(status);
  }

  if (message->HasExtension(proto::Configuration::extension)) {
    UniquePtr<proto::Configuration> configuration(
        message->ReleaseExtension(proto::Configuration::extension));
    configuration->Clear();

    proto::Configuration::Emitter* emitter = configuration->mutable_emitter();
    // FIXME(matthewtff): Remove |socket_path| as required fields go away after
    // migration to Protobuf 3.
    emitter->set_socket_path("no-op");
    for (const proto::Host& remote : conf()->coordinator().remotes()) {
      proto::Host* host = emitter->add_remotes();
      host->CopyFrom(remote);
    }

    connection->SendAsync(std::move(configuration));
    return true;
  }

  NOTREACHED();
  return false;
}

}  // namespace daemon
}  // namespace dist_clang
