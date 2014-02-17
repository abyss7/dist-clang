#include "base/locked_queue_impl.h"

#include "gtest/gtest.h"

namespace dist_clang {
namespace base {

TEST(LockedQueueTest, BasicUsage) {
  LockedQueue<int> queue;
  const int expected = 1;
  int actual;

  EXPECT_EQ(0u, queue.Size());
  ASSERT_TRUE(queue.Push(expected));
  EXPECT_EQ(1u, queue.Size());
  ASSERT_TRUE(queue.Pop(actual));
  EXPECT_EQ(expected, actual);
  EXPECT_EQ(0u, queue.Size());

  for (int i = 1; i < 10; ++i) {
    ASSERT_TRUE(queue.Push(i));
    EXPECT_EQ(static_cast<unsigned>(i), queue.Size());
  }
  for (int i = 1; i < 10; ++i) {
    ASSERT_TRUE(queue.Pop(actual));
    EXPECT_EQ(i, actual);
    EXPECT_EQ(static_cast<unsigned>(9 - i), queue.Size());
  }
}

TEST(LockedQueueTest, ExceedCapacity) {
  LockedQueue<int> queue(1);
  int actual;

  ASSERT_TRUE(queue.Push(1));
  ASSERT_FALSE(queue.Push(1));
  EXPECT_EQ(1u, queue.Size());
  ASSERT_TRUE(queue.Pop(actual));
  EXPECT_EQ(0u, queue.Size());
}

TEST(LockedQueueTest, CloseQueue) {
  LockedQueue<int> queue;
  int actual;

  ASSERT_TRUE(queue.Push(1));
  queue.Close();
  ASSERT_FALSE(queue.Push(1));
  ASSERT_TRUE(queue.Pop(actual));
  actual = 10;
  ASSERT_FALSE(queue.Pop(actual));
  EXPECT_EQ(10, actual);
  EXPECT_EQ(0u, queue.Size());
}

TEST(LockedQueueTest, MoveSemantics) {
  struct TestClass {
    TestClass() {
      ++counter();
    }
    TestClass(const TestClass& t) {
      ++counter();
    }
    TestClass(TestClass&& t) {
      // Do nothing.
    }
    TestClass& operator= (TestClass&& t) {
      return *this;
    }

    static int& counter() {
      static int counter_ = 0;
      return counter_;
    }
  };

  LockedQueue<TestClass> queue;
  TestClass expected, actual;

  ASSERT_TRUE(queue.Push(expected));
  ASSERT_TRUE(queue.Pop(actual));
  EXPECT_EQ(3, actual.counter());
}

TEST(LockedQueueTest, DISABLED_BasicMultiThreadedUsage) {
  LockedQueue<int> queue;
  // TODO: implement this.
}

}  // namespace base
}  // namespace dist_clang
