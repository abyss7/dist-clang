#pragma once

#include <base/attributes.h>
#include <base/types.h>

namespace dist_clang {
namespace base {

template <class T>
class LockedList {
 public:
  using Optional = Optional<T>;

  ~LockedList() {
    while (Pop()) {
    }
  }

  void Append(const T& obj) THREAD_SAFE {
    Node *new_node = new Node(obj, nullptr), *tail = tail_hint_,
         *expected = nullptr;
    while (!tail->next.compare_exchange_weak(expected, new_node)) {
      if (expected) {
        tail = expected;
        expected = nullptr;
      }
    }

    tail_hint_ = new_node;
  }

  void Append(T&& obj) THREAD_SAFE {
    Node *new_node = new Node(std::move(obj), nullptr), *tail = tail_hint_,
         *expected = nullptr;
    while (!tail->next.compare_exchange_weak(expected, new_node)) {
      if (expected) {
        tail = expected;
        expected = nullptr;
      }
    }

    tail_hint_ = new_node;
  }

  Optional Pop() THREAD_UNSAFE {
    if (head_.next == nullptr) {
      return Optional();
    }

    UniquePtr<Node> old_node(head_.next);
    head_.next.store(old_node->next.load());

    if (head_.next == nullptr) {
      tail_hint_ = &head_;
    }

    return std::move(old_node->obj);
  }

 private:
  struct Node {
    Node(const T& obj, Node* next) : obj(obj), next(next) {}
    Node(T&& obj, Node* next) : obj(std::move(obj)), next(next) {}

    T obj;
    Atomic<Node*> next;
  };

  Node head_{T(), nullptr};
  Atomic<Node*> tail_hint_{&head_};
};

}  // namespace base
}  // namespace dist_clang
