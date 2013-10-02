#include "net/event_loop.h"

#include <functional>

#include <signal.h>

namespace {

sigset_t BlockSignals() {
  sigset_t signal_set, old_set;

  sigfillset(&signal_set);
  pthread_sigmask(SIG_SETMASK, &signal_set, &old_set);
  return old_set;
}

void UnblockSignals(sigset_t old_set) {
  pthread_sigmask(SIG_SETMASK, &old_set, nullptr);
}

}  // namespace

namespace dist_clang {
namespace net {

EventLoop::EventLoop(size_t concurrency)
  : is_running_(false), io_threads_(std::max(concurrency, 1ul)),
    is_shutting_down_(false) {}

EventLoop::~EventLoop() {
  assert(is_shutting_down_);
}

bool EventLoop::Run() {
  bool old_running = false;
  if (!is_running_.compare_exchange_strong(old_running, true) ||
      is_shutting_down_)
    return false;

  auto old_signals = BlockSignals();

  listening_thread_ = std::thread(&EventLoop::DoListenWork, this,
                                  std::cref(is_shutting_down_));

  closing_thread_ = std::thread(&EventLoop::DoClosingWork, this,
                                std::cref(is_shutting_down_));

  for (std::thread& thread: io_threads_)
    thread = std::thread(&EventLoop::DoIOWork, this,
                         std::cref(is_shutting_down_));

  UnblockSignals(old_signals);

  return true;
}

void EventLoop::Stop() {
  bool old_running = true;
  is_shutting_down_ = true;
  if (is_running_.compare_exchange_strong(old_running, false)) {
    assert(listening_thread_.joinable());
    listening_thread_.join();
    assert(closing_thread_.joinable());
    closing_thread_.join();
    for (std::thread& thread: io_threads_) {
      assert(thread.joinable());
      thread.join();
    }
  }
}

}  // namespace net
}  // namespace dist_clang
