#pragma once

#include "net/base/types.h"

#include <functional>
#include <thread>
#include <vector>

namespace dist_clang {
namespace base {

class WorkerPool {
  public:
    using Worker = std::function<void(const volatile bool&, net::fd_t pipe)>;

    WorkerPool();
    ~WorkerPool();

    void AddWorker(const Worker& worker, unsigned count = 1);

  private:
    std::vector<std::thread> workers_;
    volatile bool is_shutting_down_;
    net::fd_t self_pipe_[2];
};

}  // namespace base
}  // namespace dist_clang
