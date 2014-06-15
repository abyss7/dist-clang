#pragma once

#include <base/aliases.h>
#include <net/base/types.h>

#include <third_party/libcxx/exported/include/atomic>
#include <third_party/libcxx/exported/include/thread>
#include <third_party/libcxx/exported/include/vector>

namespace dist_clang {
namespace base {

class WorkerPool {
 public:
  using NetWorker = Fn<void(const std::atomic<bool>&, net::fd_t)>;
  using SimpleWorker = Fn<void(const std::atomic<bool>&)>;

  explicit WorkerPool(bool force_shut_down = false);
  ~WorkerPool();

  void AddWorker(const NetWorker& worker, ui32 count = 1);
  void AddWorker(const SimpleWorker& worker, ui32 count = 1);

 private:
  std::vector<std::thread> workers_;
  std::atomic<bool> is_shutting_down_, force_shut_down_;
  net::fd_t self_pipe_[2];
};

}  // namespace base
}  // namespace dist_clang
