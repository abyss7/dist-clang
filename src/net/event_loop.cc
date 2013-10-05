#include "net/event_loop.h"

#include "net/base/utils.h"

#include <functional>

namespace dist_clang {
namespace net {

EventLoop::EventLoop(size_t concurrency)
  : is_running_(0), concurrency_(concurrency) {
}

EventLoop::~EventLoop() {
  assert(!pool_);
}

bool EventLoop::Run() {
  using namespace std::placeholders;

  int old_running = 0;
  if (!is_running_.compare_exchange_strong(old_running, 1)) {
    return false;
  }

  auto old_signals = BlockSignals();

  pool_.reset(new WorkerPool);
  pool_->AddWorker(std::bind(&EventLoop::DoListenWork, this, _1));
  pool_->AddWorker(std::bind(&EventLoop::DoClosingWork, this, _1));
  pool_->AddWorker(std::bind(&EventLoop::DoIOWork, this, _1), concurrency_);

  UnblockSignals(old_signals);

  return true;
}

void EventLoop::Stop() {
  int old_running = 1;
  if (is_running_.compare_exchange_strong(old_running, 2)) {
    pool_.reset();
  }
}

}  // namespace net
}  // namespace dist_clang
