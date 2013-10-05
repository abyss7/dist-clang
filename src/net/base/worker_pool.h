#pragma once

#include <functional>
#include <thread>
#include <vector>

#include <signal.h>

namespace dist_clang {
namespace net {

class WorkerPool {
  public:
    using Worker = std::function<void(const volatile bool&)>;

    WorkerPool();
    ~WorkerPool();

    void AddWorker(const Worker& worker, unsigned count = 1);

    const static int interrupt_signal = SIGUSR1;

  private:
    std::vector<std::thread> workers_;
    volatile bool is_shutting_down_;
};

}  // namespace net
}  // namespace dist_clang
