#include "net/base/worker_pool.h"

#include "base/assert.h"

namespace dist_clang {
namespace net {

WorkerPool::WorkerPool()
  : is_shutting_down_(false) {
}

WorkerPool::~WorkerPool() {
  is_shutting_down_ = true;
  for (auto& thread: workers_) {
    base::Assert(thread.joinable());
    pthread_kill(thread.native_handle(), interrupt_signal);
    thread.join();
  }
}

void WorkerPool::AddWorker(const Worker& worker, unsigned count) {
  base::Assert(count);
  auto closure = std::bind(worker, std::cref(is_shutting_down_));
  for (unsigned i = 0; i < count; ++i) {
    workers_.emplace_back(closure);
  }
}

}  // namespace net
}  // namespace dist_clang
