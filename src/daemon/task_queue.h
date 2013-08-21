#pragma once

#include <atomic>
#include <functional>

typedef std::function<void(void)> Closure;

class TaskQueue {
  public:
    TaskQueue();
    ~TaskQueue();

    class Node {
      public:
        Node();

        Closure task;

      private:
        friend class TaskQueue;

        constexpr static Node* const null = nullptr;

        std::atomic<Node*> next;
    };

    // List doesn't take an ownership of nodes.
    void enqueue(Node* node);
    Node* dequeue();

    unsigned long size() const;

  private:
    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;
    std::atomic_long size_;
};
