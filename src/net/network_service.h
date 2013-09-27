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
    using ConnectionCallback = std::function<void(ConnectionPtr)>;

    NetworkService();

    bool Listen(const std::string& path,
                ConnectionCallback callback,
                std::string* error = nullptr) THREAD_UNSAFE;
    bool Listen(const std::string& host, unsigned short port,
                ConnectionCallback callback,
                std::string* error = nullptr) THREAD_UNSAFE;
    ConnectionPtr Connect(const std::string& path,
                          std::string* error = nullptr) THREAD_SAFE;

    bool Run();

  private:
    // |fd| is a descriptor of a listening socket, which accepts new connection.
    void HandleNewConnection(fd_t fd, ConnectionPtr connection);

    std::unique_ptr<EventLoop> event_loop_;
    std::unordered_map<fd_t, ConnectionCallback> callbacks_;
};

}  // namespace net
}  // namespace dist_clang
