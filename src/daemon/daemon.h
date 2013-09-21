#pragma once

#include "daemon/balancer.h"
#include "daemon/thread_pool.h"
#include "net/connection_forward.h"

namespace dist_clang {

namespace net {
class NetworkService;
class Connection;
}

namespace proto {
class Error;
class LocalExecute;
class RemoteExecute;
class Universal;
}

namespace daemon {

class Configuration;

class Daemon {
  public:
    typedef proto::Error Error;
    typedef proto::LocalExecute Local;
    typedef proto::RemoteExecute Remote;

    bool Initialize(const Configuration& configuration,
                    net::NetworkService& network_service);

  private:
    void HandleNewConnection(net::ConnectionPtr connection);
    bool HandleNewMessage(net::ConnectionPtr connection,
                          const proto::Universal& message,
                          const Error& error);
    void DoLocalExecution(net::ConnectionPtr connection, const Local &execute);
    void DoLocalCompilation(net::ConnectionPtr connection, const Local& execute,
                            bool update_cache);
    bool DoRemoteCompilation(net::ConnectionPtr connection, const Error& error);

    std::unique_ptr<ThreadPool> pool_;
    std::unique_ptr<Balancer> balancer_;
};

}  // namespace daemon

}  // namespace dist_clang
