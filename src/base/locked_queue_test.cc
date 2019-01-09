#include <base/locked_queue.h>

#include <base/const_string.h>
#include <base/worker_pool.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace base {

TEST(LockedQueueTest, BasicUsage) {
  LockedQueue<int> queue;
  const int expected = 1;
  LockedQueue<int>::Optional actual;

  EXPECT_EQ(0u, queue.Size());
  ASSERT_TRUE(queue.Push(expected));
  EXPECT_EQ(1u, queue.Size());
  ASSERT_TRUE(!!(actual = queue.Pop()));
  EXPECT_EQ(expected, *actual);
  EXPECT_EQ(0u, queue.Size());

  for (int i = 1; i < 10; ++i) {
    ASSERT_TRUE(queue.Push(i));
    EXPECT_EQ(static_cast<ui32>(i), queue.Size());
  }
  for (int i = 1; i < 10; ++i) {
    ASSERT_TRUE(!!(actual = queue.Pop()));
    EXPECT_EQ(i, *actual);
    EXPECT_EQ(static_cast<ui32>(9 - i), queue.Size());
  }

  queue.Close();
}

TEST(LockedQueueTest, ExceedCapacity) {
  LockedQueue<int> queue(1);
  LockedQueue<int>::Optional actual;

  ASSERT_TRUE(queue.Push(1));
  ASSERT_FALSE(queue.Push(1));
  EXPECT_EQ(1u, queue.Size());
  ASSERT_TRUE(!!(actual = queue.Pop()));
  EXPECT_EQ(0u, queue.Size());

  queue.Close();
}

TEST(LockedQueueTest, CloseQueue) {
  LockedQueue<int> queue;
  LockedQueue<int>::Optional actual;

  ASSERT_TRUE(queue.Push(1));
  queue.Close();
  ASSERT_FALSE(queue.Push(1));
  ASSERT_TRUE(!!(actual = queue.Pop()));
  EXPECT_EQ(1, *actual);
  ASSERT_FALSE(!!(actual = queue.Pop()));
  EXPECT_EQ(0u, queue.Size());
}

TEST(LockedQueueTest, MoveSemantics) {
  struct TestClass {
    TestClass() { ++counter(); }
    TestClass(const TestClass& t) { ++counter(); }
    TestClass(TestClass&& t) {
      // Do nothing.
    }
    TestClass& operator=(TestClass&& t) { return *this; }

    static int& counter() {
      static int counter_ = 0;
      return counter_;
    }
  };

  LockedQueue<TestClass> queue;
  TestClass expected;

  ASSERT_TRUE(queue.Push(std::move(expected)));
  LockedQueue<TestClass>::Optional&& actual = queue.Pop();
  ASSERT_TRUE(!!actual);
  EXPECT_EQ(1, actual->counter());

  queue.Close();

  actual->counter() = 0;
}

TEST(LockedQueueTest, UniquePtrFriendliness) {
  class Observer {
   public:
    Observer(bool& exist) : exist_(exist) { exist_ = true; }
    ~Observer() { exist_ = false; }

   private:
    bool& exist_;
  };

  bool observer_exists = true;
  LockedQueue<UniquePtr<Observer>> queue;

  UniquePtr<Observer> ptr(new Observer(observer_exists));
  ASSERT_TRUE(queue.Push(std::move(ptr)));
  EXPECT_FALSE(ptr);
  EXPECT_TRUE(observer_exists);
  auto&& actual = queue.Pop();
  EXPECT_TRUE(observer_exists);
  ASSERT_TRUE(!!actual);
  ASSERT_TRUE(!!(*actual));

  queue.Close();
}

TEST(LockedQueueTest, BasicMultiThreadedUsage) {
  using Worker = WorkerPool::SimpleWorker;

  LockedQueue<int, true> queue(Seconds(1));
  const int number_of_tasks_to_process = 20;
  const size_t tasks_to_leave = 5;
  const int total_number_of_tasks = number_of_tasks_to_process + tasks_to_leave;
  UniquePtr<WorkerPool> workers(new WorkerPool);
  Worker worker = [&queue](const WorkerPool& pool) {
    EXPECT_TRUE(!!queue.Pop(pool, 0));
  };
  workers->AddWorker("Test worker"_l, worker, number_of_tasks_to_process);

  for (int task = 0; task < total_number_of_tasks; ++task) {
    EXPECT_TRUE(queue.Push(task));
  }

  workers.reset();
  ASSERT_EQ(tasks_to_leave, queue.Size());
  queue.Close();
}

TEST(LockedQueueTest, NotStrictSharding) {
  using Worker = WorkerPool::SimpleWorker;

  LockedQueue<int, true> queue(Seconds(1));
  const int number_of_tasks_to_process = 20;
  const size_t tasks_to_leave = 5;
  const int total_number_of_tasks = number_of_tasks_to_process + tasks_to_leave;

  const size_t initial_number_of_shards = 4;
  UniquePtr<WorkerPool> workers(new WorkerPool);
  for (size_t shard = 0; shard < initial_number_of_shards; ++shard) {
    Worker worker = [shard, &queue](const WorkerPool& pool) {
      const int tasks_per_shard =
          number_of_tasks_to_process / initial_number_of_shards;
      for (int task = 0; task < tasks_per_shard; ++task) {
        EXPECT_TRUE(
            !!queue.Pop(pool, LockedQueue<int>::NOT_STRICT_SHARDING, shard));
      }
    };
    workers->AddWorker("Test worker"_l, worker);
  }

  const size_t new_number_of_shards = 5;

  for (int task = 0; task < total_number_of_tasks; ++task) {
    EXPECT_TRUE(queue.Push(task, task % new_number_of_shards));
  }

  workers.reset();
  ASSERT_EQ(tasks_to_leave, queue.Size());
  queue.Close();
}

TEST(LockedQueueTest, StrictSharding) {
  using Worker = WorkerPool::SimpleWorker;

  LockedQueue<ui32, true> queue(Seconds(1));
  const ui32 number_of_tasks = 500u;
  constexpr const size_t number_of_shards = 4u;
  constexpr const size_t shard_with_tasks = 2u;

  static_assert(number_of_shards > shard_with_tasks,
                "Selected shard should be lower than overall number of shards");

  for (ui32 task = 0u; task < number_of_tasks; ++task) {
    EXPECT_TRUE(queue.Push(task, shard_with_tasks));
  }

  Atomic<ui32> tasks_done = {0u};
  Mutex get_tasks;
  std::condition_variable get_tasks_done;

  UniquePtr<WorkerPool> workers(new WorkerPool(true));
  for (size_t shard = 0; shard < number_of_shards; ++shard) {
    Worker worker = [&, shard](const WorkerPool& pool) {
      if (shard == shard_with_tasks) {
        for (ui32 task = 0u; task < number_of_tasks; ++task) {
          EXPECT_TRUE(!!queue.Pop(pool, number_of_tasks, shard));
          ++tasks_done;
        }
        get_tasks_done.notify_one();
      }
      while (!pool.IsShuttingDown()) {
        EXPECT_FALSE(!!queue.Pop(pool, number_of_tasks, shard));
      }
      EXPECT_FALSE(!!queue.Pop(pool, number_of_tasks, shard));
    };
    workers->AddWorker("Test worker"_l, worker);
  }

  UniqueLock lock(get_tasks);
  EXPECT_TRUE(get_tasks_done.wait_for(
      lock, Seconds(1), [&] { return tasks_done == number_of_tasks; }));

  workers.reset();
  ASSERT_EQ(0u, queue.Size());
  queue.Close();
}

TEST(LockedQueueTest, OverloadedShardSelectedOnQueueLimit) {
  using Worker = WorkerPool::SimpleWorker;

  LockedQueue<ui32, true> queue(Seconds(1));
  const ui32 number_of_tasks = 6u;
  const ui32 shard_queue_limit = 5u;
  const ui32 available_tasks = number_of_tasks - shard_queue_limit;
  constexpr const size_t number_of_shards = 2u;
  constexpr const size_t shard_without_tasks = 0u;
  constexpr const size_t shard_with_tasks = 1u;

  static_assert(number_of_shards > shard_with_tasks,
                "Selected shard should be lower than overall number of shards");

  UniquePtr<WorkerPool> workers(new WorkerPool(true));

  for (ui32 task = 0u; task < number_of_tasks; ++task) {
    EXPECT_TRUE(queue.Push(task, shard_with_tasks));
  }

  Atomic<ui32> tasks_done = {0u};
  Mutex get_tasks;
  std::condition_variable get_tasks_done;

  Worker worker = [&](const WorkerPool& pool) {
    for (size_t task = 0u; task < available_tasks; ++task) {
      EXPECT_TRUE(!!queue.Pop(pool, shard_queue_limit, shard_without_tasks));
      ++tasks_done;
    }
    get_tasks_done.notify_one();
    // Wait for pool to start shutting down.
    while (!pool.IsShuttingDown()) {
      std::this_thread::sleep_for(Seconds(1));
    }

    // Popping from shard should not work on shutting down pool.
    EXPECT_FALSE(!!queue.Pop(pool, shard_queue_limit, shard_with_tasks));

    // Popping from overloaded shard also should not work on shutting down pool.
    EXPECT_FALSE(!!queue.Pop(
        pool, shard_queue_limit - 1u, shard_without_tasks));
  };
  workers->AddWorker("Test worker"_l, worker);

  UniqueLock lock(get_tasks);
  EXPECT_TRUE(get_tasks_done.wait_for(
      lock, Seconds(1), [&] { return tasks_done == available_tasks; }));

  workers.reset();
  ASSERT_EQ(shard_queue_limit, queue.Size());
  queue.Close();
}

TEST(LockedQueueIndexTest, BasicUsage) {
  List<int> list;
  LockedQueue<int>::Index index;

  const ui32 shard = 4u;

  auto inserted_item = list.insert(list.end(), 1);
  index.Put(inserted_item, shard);
  EXPECT_EQ(shard + 1, index.index_.size());
  EXPECT_EQ(1u, index.index_[shard].tasks.size());
  EXPECT_EQ(1u, index.reverse_index_.size());
  EXPECT_EQ(1u, index.reverse_index_.count(&*inserted_item));

  EXPECT_EQ(inserted_item, index.GetWithHint(shard, list.begin()));

  // Index shouldn't shrink on elements removal.
  EXPECT_EQ(shard + 1, index.index_.size());
  EXPECT_EQ(0u, index.index_[shard].tasks.size());
  EXPECT_EQ(0u, index.reverse_index_.size());
  EXPECT_EQ(0u, index.reverse_index_.count(&*inserted_item));
}

TEST(LockedQueueIndexTest, GetWithHintFromHead) {
  List<int> list;
  LockedQueue<int>::Index index;

  const ui32 shard = 4u;
  const ui32 empty_shard = 5u;

  auto inserted_item = list.insert(list.end(), 1);
  index.Put(inserted_item, shard);
  EXPECT_EQ(shard + 1, index.index_.size());
  EXPECT_EQ(1u, index.index_[shard].tasks.size());
  EXPECT_EQ(1u, index.reverse_index_.size());
  EXPECT_EQ(1u, index.reverse_index_.count(&*inserted_item));

  EXPECT_EQ(inserted_item, index.GetWithHint(empty_shard, list.begin()));

  // Index shouldn't shrink on elements removal.
  EXPECT_EQ(shard + 1, index.index_.size());
  EXPECT_EQ(0u, index.index_[shard].tasks.size());
  EXPECT_EQ(0u, index.reverse_index_.size());
  EXPECT_EQ(0u, index.reverse_index_.count(&*inserted_item));
}

TEST(LockedQueueIndexTest, GetStrict) {
  List<int> list;
  LockedQueue<int>::Index index;

  const ui32 shard = 4u;

  auto inserted_item = list.insert(list.end(), 1);

  ASSERT_ANY_THROW(index.GetStrict(shard));

  index.Put(inserted_item, shard);
  EXPECT_EQ(shard + 1, index.index_.size());
  EXPECT_EQ(1u, index.index_[shard].tasks.size());
  EXPECT_EQ(1u, index.reverse_index_.size());
  EXPECT_EQ(1u, index.reverse_index_.count(&*inserted_item));

  EXPECT_EQ(inserted_item, index.GetStrict(shard));
  ASSERT_ANY_THROW(index.GetStrict(shard - 1u));
}

TEST(LockedQueueIndexTest, ShardIndexGrowsOnPut) {
  List<int> list;
  LockedQueue<int>::Index index;

  const ui32 shard1 = 4u;
  const ui32 shard2 = 5u;

  auto inserted_item = list.insert(list.end(), 1);
  index.Put(inserted_item, shard1);
  EXPECT_EQ(shard1 + 1, index.index_.size());
  EXPECT_EQ(1u, index.index_[shard1].tasks.size());
  EXPECT_EQ(1u, index.reverse_index_.count(&*inserted_item));

  ASSERT_ANY_THROW(index.NotifyShard(shard2));

  inserted_item = list.insert(list.end(), 5);
  index.Put(inserted_item, shard2);
  EXPECT_EQ(shard2 + 1, index.index_.size());
  EXPECT_EQ(1u, index.index_[shard1].tasks.size());
  EXPECT_EQ(1u, index.index_[shard2].tasks.size());
  EXPECT_EQ(1u, index.reverse_index_.count(&*inserted_item));
}

TEST(LockedQueueIndexTest, ShardIndexGrowsOnOverloadedSearch) {
  List<int> list;
  LockedQueue<int>::Index index;

  const ui32 shard1 = 4u;
  const ui32 shard2 = 5u;

  auto inserted_item = list.insert(list.end(), 1);
  index.Put(inserted_item, shard1);
  EXPECT_EQ(shard1 + 1, index.index_.size());
  EXPECT_EQ(1u, index.index_[shard1].tasks.size());
  EXPECT_EQ(1u, index.reverse_index_.count(&*inserted_item));

  ASSERT_ANY_THROW(index.ShardIsEmpty(shard2));
  ASSERT_ANY_THROW(index.GetStrict(shard2));

  const ui32 max_queue_size = 3u;

  EXPECT_EQ(shard2, index.MaybeOverloadedShard(max_queue_size, shard2));
  EXPECT_EQ(shard2 + 1, index.index_.size());
  EXPECT_EQ(1u, index.index_[shard1].tasks.size());
  EXPECT_EQ(1u, index.reverse_index_.count(&*inserted_item));
  EXPECT_EQ(0u, index.index_[shard2].tasks.size());
}

TEST(LockedQueueIndexTest, MaybeOverloadedReturnsOverloadedShard) {
  List<int> list;
  LockedQueue<int>::Index index;

  const ui32 shard1 = 4u;
  const ui32 shard2 = 5u;

  const ui32 max_queue_size = 3u;

  List<int>::iterator inserted_item;
  for (ui32 task = 1; task < 2 * max_queue_size; ++task) {
    inserted_item = list.insert(list.end(), task);
    index.Put(inserted_item, shard1);
    EXPECT_EQ(shard1 + 1, index.index_.size());
    EXPECT_EQ(task, index.index_[shard1].tasks.size());
    EXPECT_EQ(1u, index.reverse_index_.count(&*inserted_item));
  }

  ASSERT_ANY_THROW(index.ShardIsEmpty(shard2));
  ASSERT_ANY_THROW(index.GetStrict(shard2));

  EXPECT_EQ(shard1, index.MaybeOverloadedShard(max_queue_size, shard2));
  // Check that |MaybeOverloadedShard| returns really overloaded shard, even if
  // we hint to |shard2|.

  EXPECT_EQ(shard2 + 1, index.index_.size());
  EXPECT_EQ(0u, index.index_[shard2].tasks.size());

  inserted_item = list.insert(list.end(), max_queue_size * 3u);
  index.Put(inserted_item, shard2);

  EXPECT_EQ(shard2, index.MaybeOverloadedShard(max_queue_size, shard2));
  // But if there's at least one task in hinted shard, |MaybeOverloadedShard|
  // should favor the hint.

  EXPECT_EQ(1u, index.index_[shard2].tasks.size());
}

}  // namespace base
}  // namespace dist_clang
