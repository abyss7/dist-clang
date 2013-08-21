#pragma once

#include "thread_pool.h"

#include <set>
#include <string>
#include <thread>
#include <vector>

class Server {
  public:
    explicit Server(size_t concurrency = std::thread::hardware_concurrency());

    // Listen on a Unix Domain Socket.
    bool Listen(const std::string& path, std::string* error);

    // Listen on a TCP Socket.
    bool Listen(const std::string& host, short int port, std::string* error);

    bool Run();

  private:
    void DoWork(int fd);

    std::set<int> listen_fds_;
    size_t concurrency_;
    ThreadPool thread_pool_;
    std::vector<std::thread> network_threads_;
};
