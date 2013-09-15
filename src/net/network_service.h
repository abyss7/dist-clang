#pragma once

#include "base/attributes.h"
#include "net/base/types.h"
#include "net/event_loop.h"
#include "proto/remote.pb.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace dist_clang {
namespace net {

class NetworkService {
  public:
    typedef std::function<void(ConnectionPtr)> ConnectionCallback;

    NetworkService();

    bool Listen(const std::string& path,
                ConnectionCallback callback,
                proto::Error* error) THREAD_UNSAFE;
    ConnectionPtr Connect(const std::string& path,
                          proto::Error* error) THREAD_SAFE;

    bool Run();

  private:
    // |fd| is a descriptor of a listening socket, which accepts new connection.
    void HandleNewConnection(fd_t fd, ConnectionPtr connection);

    std::unique_ptr<EventLoop> event_loop_;
    std::unordered_map<fd_t, ConnectionCallback> callbacks_;
};

}  // namespace net
}  // namespace dist_clang
