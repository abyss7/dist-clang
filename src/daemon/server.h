#pragma once

#include "daemon/epoll_set.h"
#include "daemon/thread_pool.h"

#include <string>
#include <thread>

class Server {
  public:
    explicit Server(size_t concurrency = std::thread::hardware_concurrency(),
                    size_t pool_size = 1024);

    // Listen on a Unix Domain Socket.
    bool Listen(const std::string& path, std::string* error);

    // Listen on a TCP Socket.
    bool Listen(const std::string& host, short int port, std::string* error);

    bool Run();

  private:
    void DoWork();
    void HandleMessage(int fd, bool close_after_use);

    EpollSet epoll_set_;
    ThreadPool thread_pool_;
    std::vector<std::thread> network_threads_;
    bool is_running_;
};
