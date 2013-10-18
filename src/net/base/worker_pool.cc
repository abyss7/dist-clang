#include "net/base/worker_pool.h"

#include "base/assert.h"

#include <unistd.h>

namespace dist_clang {
namespace net {

WorkerPool::WorkerPool()
  : is_shutting_down_(false) {
  // TODO: check somehow for error in the |pipe()| call.
  pipe(self_pipe_);
}

WorkerPool::~WorkerPool() {
  is_shutting_down_ = true;
  for (auto& thread: workers_) {
    base::Assert(thread.joinable());
    close(self_pipe_[1]);
    thread.join();
  }
  close(self_pipe_[0]);
}

void WorkerPool::AddWorker(const Worker& worker, unsigned count) {
  base::Assert(count);
  auto closure = std::bind(worker, std::cref(is_shutting_down_), self_pipe_[0]);
  for (unsigned i = 0; i < count; ++i) {
    workers_.emplace_back(closure);
  }
}

}  // namespace net
}  // namespace dist_clang
