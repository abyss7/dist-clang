#pragma once

#include <base/aliases.h>
#include <base/const_string.h>

#include STL(thread)

#include <signal.h>

namespace dist_clang {
namespace base {

class Thread {
 public:
  using id = std::thread::id;

  Thread() = default;

  template <class F, class... Args>
  explicit Thread(Literal name, F&& f, Args&&... args) {
    auto old_set = BlockSignals();
    thread_ = std::thread(f, args...);
#if defined(OS_LINUX)
    pthread_setname_np(thread_.native_handle(), name);
#elif defined(OS_MACOSX)
// TODO: implement the thread naming.
//       See http://stackoverflow.com/q/31897364/377393
#endif
    UnblockSignals(old_set);
  }

  inline id get_id() const { return thread_.get_id(); }
  inline void detach() { thread_.detach(); }
  inline void join() { thread_.join(); }
  inline bool joinable() const { return thread_.joinable(); }
  inline void swap(Thread& other) { thread_.swap(other.thread_); }

 private:
  inline sigset_t BlockSignals() {
    sigset_t signal_set, old_set;

    sigfillset(&signal_set);
    sigdelset(&signal_set, SIGPROF);
    sigdelset(&signal_set, SIGSEGV);

    // This should help to avoid program hanging on Mac.
    // See http://stackoverflow.com/q/31947384/377393
    sigdelset(&signal_set, SIGABRT);

    pthread_sigmask(SIG_SETMASK, &signal_set, &old_set);
    return old_set;
  }

  inline void UnblockSignals(sigset_t old_set) {
    pthread_sigmask(SIG_SETMASK, &old_set, nullptr);
  }

  std::thread thread_;
};

}  // namespace base
}  // namespace dist_clang
