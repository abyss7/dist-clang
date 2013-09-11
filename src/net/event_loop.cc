#include "net/event_loop.h"

#include "net/connection.h"

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
  : is_running_(false), incoming_threads_(concurrency + 1),
    outgoing_threads_(concurrency + 1), is_shutting_down_(false) {}

EventLoop::~EventLoop() {
  is_shutting_down_ = true;

  if (is_running_) {
    listening_thread_.join();
    for (std::thread& thread: incoming_threads_) {
      assert(thread.joinable());
      thread.join();
    }
    for (std::thread& thread: outgoing_threads_) {
      assert(thread.joinable());
      thread.join();
    }
  }
}

bool EventLoop::Run() {
  if (is_running_ || is_shutting_down_)
    return false;

  auto old_signals = BlockSignals();

  listening_thread_ = std::thread(&EventLoop::DoListenWork, this,
                                  std::cref(is_shutting_down_));

  for (std::thread& thread: incoming_threads_)
    thread = std::thread(&EventLoop::DoIncomingWork, this,
                         std::cref(is_shutting_down_));

  for (std::thread& thread: outgoing_threads_)
    thread = std::thread(&EventLoop::DoOutgoingWork, this,
                         std::cref(is_shutting_down_));

  UnblockSignals(old_signals);

  is_running_ = true;
  return true;
}

int EventLoop::GetConnectionDescriptor(const ConnectionPtr connection) const {
  return connection->fd_;
}

void EventLoop::ConnectionCanRead(ConnectionPtr connection) {
  connection->CanRead();
}

void EventLoop::ConnectionCanSend(ConnectionPtr connection) {
  connection->CanSend();
}

}  // namespace net
}  // namespace dist_clang
