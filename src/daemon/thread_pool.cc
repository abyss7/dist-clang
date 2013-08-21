#include "thread_pool.h"

#include "base/futex.h"

#include <algorithm>

ThreadPool::ThreadPool(size_t capacity, size_t concurrency)
  : workers_(concurrency), is_empty_(true), is_shutting_down_(false) {
  for (auto i = capacity; i > 0; --i) {
    TaskQueue::Node* new_node = new TaskQueue::Node;
    free_nodes_.enqueue(new_node);
  }
}

ThreadPool::~ThreadPool() {
  is_shutting_down_ = true;

  for (auto it = workers_.begin(); it != workers_.end(); ++it)
    it->join();

  TaskQueue::Node* node;
  do {
    node = free_nodes_.dequeue();
    delete node;
  } while (node);
}

void ThreadPool::Run() {
  auto lambda = [this](std::thread& thread) {
    std::thread tmp(&ThreadPool::DoWork, this);
    thread.swap(tmp);
  };
  std::for_each(workers_.begin(), workers_.end(), lambda);
}

bool ThreadPool::Push(const Closure& task) {
  if (is_shutting_down_)
    return false;

  TaskQueue::Node* node = free_nodes_.dequeue();
  if (!node)
    return false;

  node->task = task;
  busy_nodes_.enqueue(node);

  is_empty_ = false;
  wake_up(is_empty_, 1);

  return true;
}

void ThreadPool::DoWork() {
  do {
    if (try_to_wait(is_empty_, true) == ERROR ||
        (is_empty_ && is_shutting_down_))
      break;

    TaskQueue::Node* node = busy_nodes_.dequeue();
    if (!node)
      is_empty_ = true;

    node->task();
    free_nodes_.enqueue(node);
  } while (true);
}
