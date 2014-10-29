#pragma once

#include <base/assert.h>

#include <third_party/libcxx/exported/include/condition_variable>
#include <third_party/libcxx/exported/include/experimental/optional>

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
    ~State() {
      if (async.joinable()) {
        async.join();
      }
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool fulfilled = false;
    T value;
    Thread async;
  };

  Future(std::shared_ptr<State> state) : state_(state) { CHECK(state_); }

  std::shared_ptr<State> state_;
};

template <class T>
class Promise {
  using StatePtr = std::shared_ptr<typename Future<T>::State>;

 public:
  using Optional = std::experimental::optional<Future<T>>;

  // The |default_value| is set on object's destruction, if no other value was
  // ever set.
  Promise(const T& default_value)
      : state_(new typename Future<T>::State), on_exit_value_(default_value) {}
  Promise(Promise<T>&& other) = default;
  ~Promise() {
    if (state_) {
      SetValue(on_exit_value_);
    }
  }

  Optional GetFuture() {
    DCHECK(state_);
    return Future<T>(state_);
  }

  void SetValue(const T& value) { SetStateValue(state_, value); }

  void SetValue(Fn<T(void)> fn) {
    DCHECK(state_);
    UniqueLock lock(state_->mutex);
    if (!state_->fulfilled && !state_->async.joinable()) {
      state_->async = Thread(&SetStateValue, state_, fn());
    }
  }

 private:
  static void SetStateValue(StatePtr state, const T& value) {
    DCHECK(state);
    UniqueLock lock(state->mutex);
    if (!state->fulfilled && !state->async.joinable()) {
      state->value = value;
      state->fulfilled = true;
      state->condition.notify_all();
    }
  }

  StatePtr state_;
  T on_exit_value_;
};

}  // namespace base
}  // namespace dist_clang
