#pragma once

#include "net/base/types.h"

#include <functional>
#include <thread>
#include <vector>

namespace dist_clang {
namespace net {

class WorkerPool {
  public:
    using Worker = std::function<void(const volatile bool&, fd_t self_pipe)>;

    WorkerPool();
    ~WorkerPool();

    void AddWorker(const Worker& worker, unsigned count = 1);

  private:
    std::vector<std::thread> workers_;
    volatile bool is_shutting_down_;
    fd_t self_pipe_[2];
};

}  // namespace net
}  // namespace dist_clang
