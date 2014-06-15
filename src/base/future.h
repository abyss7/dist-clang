#pragma once

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
  void Wait();
  const T& GetValue() const;

  operator bool() const;

 private:
  friend class Promise<T>;

  struct State {
    std::mutex mutex;
    std::condition_variable condition;
    bool fulfilled = false;
    T value;
  };

  Future(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;
};

template <class T>
class Promise {
 public:
  using Optional = std::experimental::optional<Future<T>>;

  // The |default_value| is set on object's destruction, if no other value
  // was ever set.
  Promise(const T& default_value);
  Promise(Promise<T>&& other) = default;
  ~Promise();

  Optional GetFuture();
  void SetValue(const T& value);

 private:
  std::shared_ptr<typename Future<T>::State> state_;
  T on_exit_value_;
};

}  // namespace base
}  // namespace dist_clang
