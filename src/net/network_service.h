#pragma once

#include "net/connection_forward.h"
#include "net/epoll_event_loop.h"

#include <functional>
#include <memory>
#include <string>

namespace dist_clang {
namespace net {

class NetworkService {
  public:
    typedef std::function<void(ConnectionPtr)> ConnectionCallback;

    NetworkService();

    bool Listen(const std::string& path,
                ConnectionCallback callback,
                std::string* error);
    ConnectionPtr Connect(const std::string& path, std::string* error);

    bool Run();

  private:
    // |fd| is a descriptor of listening socket, which accepted new connection.
    void HandleNewConnection(int fd, ConnectionPtr connection);

    std::unique_ptr<EpollEventLoop> event_loop_;
};

}  // namespace net
}  // namespace dist_clang
