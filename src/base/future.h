#pragma once

#include <base/assert.h>
#include <base/thread.h>

#include STL(condition_variable)
#include STL(experimental/optional)

namespace dist_clang {
namespace base {

// The |std::promise| and |std::future| doesn't suite because, when the promise
// is destroyed before setting any value (the broken promise situation) the
// future triggers an exception - and we don't use exceptions.

template <class T>
class Promise;

template <class T>
class Future {
 public:
  void Wait() {
    UniqueLock lock(state_->mutex);
    state_->condition.wait(lock, [this] { return state_->fulfilled; });
  }

  const T& GetValue() const { return state_->value; }

  operator bool() const { return state_->fulfilled; }

 private:
  friend class Promise<T>;

  struct State {
    std::mutex mutex;
    std::condition_variable condition;
    bool fulfilled = false;
    Thread::id async_id;
    T value;
  };

  Future(SharedPtr<State> state, SharedPtr<Thread> async)
      : state_(state), async_(async) {
    CHECK(state_);
  }

  SharedPtr<State> state_;
  SharedPtr<Thread> async_;
};

template <class T>
class Promise {
  using StatePtr = SharedPtr<typename Future<T>::State>;

 public:
  // We need an |Optional| to be able to declare empty futures - at least in
  // tests.
  using Optional = std::experimental::optional<Future<T>>;

  // The |default_value| is set on object's destruction, if no other value was
  // ever set.
  Promise(const T& default_value)
      : state_(new typename Future<T>::State),
        async_(new Thread,
               [](Thread* thread) {
                 if (thread->joinable()) {
                   thread->join();
                 }
                 delete thread;
               }),
        on_exit_value_(default_value) {}
  Promise(Promise<T>&& other) = default;
  ~Promise() {
    if (state_) {
      SetValue(on_exit_value_);
    }
  }

  Optional GetFuture() { return Future<T>(state_, async_); }

  void SetValue(const T& value) {
    DCHECK(state_);
    SetStateValue(state_, value);
  }

  void SetValue(Fn<T(void)> fn) {
    DCHECK(state_);
    UniqueLock lock(state_->mutex);
    if (!state_->fulfilled && state_->async_id == Thread::id()) {
      Thread("Promise Set Value"_l, [fn](StatePtr state) {
        SetStateValue(state, fn());
      }, state_).swap(*async_);
      state_->async_id = async_->get_id();
    }
  }

 private:
  static void SetStateValue(StatePtr state, const T& value) {
    DCHECK(state);
    UniqueLock lock(state->mutex);
    if (!state->fulfilled && (state->async_id == Thread::id() ||
                              state->async_id == std::this_thread::get_id())) {
      state->value = value;
      state->fulfilled = true;
      state->condition.notify_all();
    }
  }

  StatePtr state_;
  SharedPtr<Thread> async_;
  T on_exit_value_;
};

}  // namespace base
}  // namespace dist_clang
