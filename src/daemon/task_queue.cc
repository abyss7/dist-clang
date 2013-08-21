#include "task_queue.h"

#include <cassert>

TaskQueue::TaskQueue()
  : size_(0) {
  auto new_node = new Node;
  head_ = new_node;
  tail_ = new_node;
}

TaskQueue::~TaskQueue() {
  assert(head_ == tail_);
  assert(size_ == 0);
  delete static_cast<Node*>(head_);
}

void TaskQueue::enqueue(Node *node) {
  if (node == Node::null)
    return;

  assert(node->next == Node::null);

  Node *tail, *next, *temp = Node::null;
  while(true) {
    tail = tail_.load();
    next = tail->next.load();
    if (!next && tail->next.compare_exchange_weak(temp, node))
      break;
    else if (next)
      tail_.compare_exchange_weak(tail, next);
  }
  tail_.compare_exchange_strong(tail, node);
  ++size_;
}

TaskQueue::Node* TaskQueue::dequeue() {
  Node *head, *tail, *next;
  while(true) {
    head = head_.load();
    tail = tail_.load();
    next = head->next.load();
    if (head == tail) {
      if (!next)
        return Node::null;
      tail_.compare_exchange_weak(tail, next);
    }
    else {
      head->task = next->task;
      if (head_.compare_exchange_weak(head, next))
        break;
    }
  }
  head->next.store(Node::null);
  --size_;
  return head;
}

size_t TaskQueue::size() const {
  return size_;
}

TaskQueue::Node::Node()
  : next(null) {}
