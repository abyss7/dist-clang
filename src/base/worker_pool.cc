#include "base/worker_pool.h"

#include "base/assert.h"

#include <unistd.h>

namespace dist_clang {
namespace base {

WorkerPool::WorkerPool(bool force_shut_down)
    : is_shutting_down_(false), force_shut_down_(force_shut_down) {
  // TODO: check somehow for error in the |pipe()| call.
  pipe(self_pipe_);
}

WorkerPool::~WorkerPool() {
  if (force_shut_down_) {
    is_shutting_down_ = true;
    close(self_pipe_[1]);
  }
  for (auto& thread : workers_) {
    DCHECK(thread.joinable());
    thread.join();
  }
  if (!force_shut_down_) {
    is_shutting_down_ = true;
    close(self_pipe_[1]);
  }
  close(self_pipe_[0]);
}

void WorkerPool::AddWorker(const NetWorker& worker, ui32 count) {
  CHECK(count);
  auto closure = std::bind(worker, std::cref(is_shutting_down_), self_pipe_[0]);
  for (ui32 i = 0; i < count; ++i) {
    workers_.emplace_back(closure);
  }
}

void WorkerPool::AddWorker(const SimpleWorker& worker, ui32 count) {
  CHECK(count);
  auto closure = std::bind(worker, std::cref(is_shutting_down_));
  for (ui32 i = 0; i < count; ++i) {
    workers_.emplace_back(closure);
  }
}

}  // namespace base
}  // namespace dist_clang
