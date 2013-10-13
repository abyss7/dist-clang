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
    using ConnectCallback =
        std::function<void(ConnectionPtr, const std::string&)>;

    explicit NetworkService(
        size_t concurrency = std::thread::hardware_concurrency());
    ~NetworkService();

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

    ConnectionPtr ConnectSync(
        const std::string& path,
        std::string* error = nullptr) THREAD_SAFE;
    ConnectionPtr ConnectSync(
        EndPointPtr end_point,
        std::string* error = nullptr) THREAD_SAFE;
    bool ConnectAsync(
        const EndPointPtr& end_point,
        ConnectCallback callback,
        std::string* error = nullptr) THREAD_SAFE;

  private:
    using CallbackPair = std::pair<ConnectCallback, EndPointPtr>;

    // |fd| is a descriptor of a listening socket, which accepts new connection.
    void HandleNewConnection(fd_t fd, ConnectionPtr connection);
    void DoConnectWork(const volatile bool& is_shutting_down);

    int epoll_fd_;
    std::unique_ptr<EventLoop> event_loop_;
    std::unordered_map<fd_t, ListenCallback> listen_callbacks_;

    size_t concurrency_;
    std::mutex connect_mutex_;
    std::unordered_map<fd_t, CallbackPair> connect_callbacks_;
    std::unique_ptr<WorkerPool> pool_;
};

}  // namespace net
}  // namespace dist_clang
