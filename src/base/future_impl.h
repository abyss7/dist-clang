#pragma once

#include "base/future.h"

#include "base/assert.h"

namespace dist_clang {
namespace base {

template <class T>
void Future<T>::Wait() {
  std::unique_lock<std::mutex> lock(state_->mutex);
  state_->condition.wait(lock, [this]{ return state_->fulfilled; });
}

template <class T>
const T& Future<T>::GetValue() const {
  return state_->value;
}

template <class T>
Future<T>::operator bool() const {
  return state_->fulfilled;
}

template <class T>
Future<T>::Future(std::shared_ptr<State> state)
  : state_(state) {
  CHECK(state_);
}

template <class T>
Promise<T>::Promise(const T& default_value)
  : state_(new typename Future<T>::State), on_exit_value_(default_value) {
}

template <class T>
Promise<T>::~Promise() {
  SetValue(on_exit_value_);
}

template <class T>
typename Promise<T>::Optional Promise<T>::GetFuture() {
  if (!state_) {
    return Optional();
  }

  return Future<T>(state_);
}

template <class T>
void Promise<T>::SetValue(const T& value) {
  if (state_) {
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (!state_->fulfilled) {
      state_->value = value;
      state_->fulfilled = true;
      state_->condition.notify_all();
    }
  }
}

}  // namespace base
}  // namespace dist_clang
