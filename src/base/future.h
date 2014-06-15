#pragma once

#include <base/assert.h>

#include <condition_variable>
#include <experimental/optional>
#include <mutex>

namespace dist_clang {
namespace base {

template <class T>
class Promise;

template <class T>
class Future {
 public:
  void Wait() {
    std::unique_lock<std::mutex> lock(state_->mutex);
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
    T value;
  };

  Future(std::shared_ptr<State> state) : state_(state) { CHECK(state_); }

  std::shared_ptr<State> state_;
};

template <class T>
class Promise {
 public:
  using Optional = std::experimental::optional<Future<T>>;

  // The |default_value| is set on object's destruction, if no other value
  // was ever set.
  Promise(const T& default_value)
      : state_(new typename Future<T>::State), on_exit_value_(default_value) {}
  Promise(Promise<T>&& other) = default;
  ~Promise() { SetValue(on_exit_value_); }

  Optional GetFuture() {
    if (!state_) {
      return Optional();
    }

    return Future<T>(state_);
  }
  void SetValue(const T& value) {
    if (state_) {
      std::unique_lock<std::mutex> lock(state_->mutex);
      if (!state_->fulfilled) {
        state_->value = value;
        state_->fulfilled = true;
        state_->condition.notify_all();
      }
    }
  }

 private:
  std::shared_ptr<typename Future<T>::State> state_;
  T on_exit_value_;
};

}  // namespace base
}  // namespace dist_clang
