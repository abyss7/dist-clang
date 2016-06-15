#include <base/worker_pool.h>

#include <base/assert.h>
#include <base/thread.h>

namespace dist_clang {
namespace base {

const std::chrono::duration<double, std::ratio<1>> WorkerPool::ZERO_DURATION
    = std::chrono::duration<double, std::ratio<1>>::zero();

WorkerPool::WorkerPool(bool force_shut_down)
    : is_shutting_down_(false), force_shut_down_(force_shut_down) {
  // TODO: check somehow for error in the |pipe()| call.
}

WorkerPool::~WorkerPool() {
  if (force_shut_down_) {
    is_shutting_down_ = true;
    shutdown_condition_.notify_all();
    self_[1].Close();
  }
  for (auto& thread : workers_) {
    DCHECK(thread.joinable());
    thread.join();
  }
  if (!force_shut_down_) {
    is_shutting_down_ = true;
    shutdown_condition_.notify_all();
  }
}

void WorkerPool::AddWorker(Literal name, const NetWorker& worker, ui32 count) {
  CHECK(count);
  auto closure = [this, worker] { worker(*this, self_[0]); };
  for (ui32 i = 0; i < count; ++i) {
    workers_.emplace_back(name, closure);
  }
}

void WorkerPool::AddWorker(Literal name, const SimpleWorker& worker,
                           ui32 count) {
  CHECK(count);
  auto closure = [this, worker] { worker(*this); };
  for (ui32 i = 0; i < count; ++i) {
    workers_.emplace_back(name, closure);
  }
}

bool WorkerPool::WaitUntilShutdown(
    const std::chrono::duration<double, std::ratio<1>>& duration) {
  if (is_shutting_down_) {
    return true;
  }

  if (duration == ZERO_DURATION) {
    return is_shutting_down_;
  }

  UniqueLock lock(shutdown_mutex_);
  return shutdown_condition_.wait_for(
      lock, duration, [this]() -> bool { return is_shutting_down_; });
}

}  // namespace base
}  // namespace dist_clang
