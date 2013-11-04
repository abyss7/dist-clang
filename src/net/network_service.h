#pragma once

#include "base/attributes.h"
#include "net/base/types.h"
#include "net/event_loop.h"
#include "proto/remote.pb.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dist_clang {
namespace net {

class NetworkService {
  public:
    using ListenCallback = std::function<void(ConnectionPtr)>;

    NetworkService();

    // We need method |Run()| to allow user to add all listening sockets in
    // a non-threadsafe way, thus, prevent locking inside |HandleNewConnection|.
    bool Run() THREAD_UNSAFE;

    bool Listen(
        const std::string& path,
        ListenCallback callback,
        std::string* error = nullptr) THREAD_UNSAFE;
    bool Listen(
        const std::string& host,
        unsigned short port,
        ListenCallback callback,
        std::string* error = nullptr) THREAD_UNSAFE;

    ConnectionPtr Connect(
        const std::string& path,
        std::string* error = nullptr) THREAD_SAFE;
    ConnectionPtr Connect(
        EndPointPtr end_point,
        std::string* error = nullptr) THREAD_SAFE;

  private:
    // |fd| is a descriptor of a listening socket, which accepts new connection.
    void HandleNewConnection(fd_t fd, ConnectionPtr connection);

    std::unique_ptr<EventLoop> event_loop_;
    std::unordered_map<fd_t, ListenCallback> listen_callbacks_;
};

}  // namespace net
}  // namespace dist_clang
