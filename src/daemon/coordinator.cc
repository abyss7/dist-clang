#include <daemon/coordinator.h>

#include <base/assert.h>
#include <base/logging.h>
#include <net/connection_impl.h>

#include <base/using_log.h>

namespace dist_clang {
namespace daemon {

Coordinator::Coordinator(const proto::Configuration& configuration)
    : BaseDaemon(configuration),
      local_(configuration.coordinator().local()),
      remotes_(configuration.coordinator().remotes().begin(),
               configuration.coordinator().remotes().end()) {
  CHECK(configuration.has_coordinator());
}

bool Coordinator::Initialize() {
  String error;
  if (!Listen(local_.host(), local_.port(), local_.ipv6(), &error)) {
    LOG(ERROR) << "[Coordinator] Failed to listen on " << local_.host() << ":"
               << local_.port() << " : " << error;
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

    proto::Configuration::Emitter* emitter = configuration->mutable_emitter();
    emitter->set_socket_path("no-op");
    for (const proto::Host& remote : remotes_) {
      proto::Host* host = emitter->add_remotes();
      *host = remote;
    }

    connection->SendAsync(std::move(configuration));
    return true;
  }

  NOTREACHED();
  return false;
}

}  // namespace daemon
}  // namespace dist_clang
