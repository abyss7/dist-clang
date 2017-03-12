#include <base/locked_queue.h>

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
  actual = 10;
  ASSERT_FALSE(!!(actual = queue.Pop()));
  EXPECT_EQ(10, *actual);
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

TEST(LockedQueueTest, DISABLED_BasicMultiThreadedUsage) {
  LockedQueue<int> queue;
  // TODO: implement this test.
}

TEST(LockedQueueIndexTest, BasicUsage) {
  List<int> list;
  LockedQueue<int>::Index index;

  const ui32 shard = 4u;

  auto inserted_item = list.insert(list.end(), 1);
  index.Put(inserted_item, shard);
  EXPECT_EQ(shard + 1, index.index_.size());
  EXPECT_EQ(1u, index.index_[shard].size());
  EXPECT_EQ(1u, index.reverse_index_.size());
  EXPECT_EQ(1u, index.reverse_index_.count(&*inserted_item));

  EXPECT_EQ(inserted_item, index.Get(shard, list.begin()));

  // Index shouldn't shrink on elements removal.
  EXPECT_EQ(shard + 1, index.index_.size());
  EXPECT_EQ(0u, index.index_[shard].size());
  EXPECT_EQ(0u, index.reverse_index_.size());
  EXPECT_EQ(0u, index.reverse_index_.count(&*inserted_item));
}

TEST(LockedQueueIndexTest, GetFromHead) {
  List<int> list;
  LockedQueue<int>::Index index;

  const ui32 shard = 4u;
  const ui32 empty_shard = 5u;

  auto inserted_item = list.insert(list.end(), 1);
  index.Put(inserted_item, shard);
  EXPECT_EQ(shard + 1, index.index_.size());
  EXPECT_EQ(1u, index.index_[shard].size());
  EXPECT_EQ(1u, index.reverse_index_.size());
  EXPECT_EQ(1u, index.reverse_index_.count(&*inserted_item));

  EXPECT_EQ(inserted_item, index.Get(empty_shard, list.begin()));

  // Index shouldn't shrink on elements removal.
  EXPECT_EQ(shard + 1, index.index_.size());
  EXPECT_EQ(0u, index.index_[shard].size());
  EXPECT_EQ(0u, index.reverse_index_.size());
  EXPECT_EQ(0u, index.reverse_index_.count(&*inserted_item));
}

TEST(LockedQueueIndexTest, ShardIndexGrows) {
  List<int> list;
  LockedQueue<int>::Index index;

  const ui32 shard1 = 4u;
  const ui32 shard2 = 5u;

  auto inserted_item = list.insert(list.end(), 1);
  index.Put(inserted_item, shard1);
  EXPECT_EQ(shard1 + 1, index.index_.size());
  EXPECT_EQ(1u, index.index_[shard1].size());
  EXPECT_EQ(1u, index.reverse_index_.count(&*inserted_item));

  inserted_item = list.insert(list.end(), 5);
  index.Put(inserted_item, shard2);
  EXPECT_EQ(shard2 + 1, index.index_.size());
  EXPECT_EQ(1u, index.index_[shard1].size());
  EXPECT_EQ(1u, index.index_[shard2].size());
  EXPECT_EQ(1u, index.reverse_index_.count(&*inserted_item));
}

}  // namespace base
}  // namespace dist_clang
