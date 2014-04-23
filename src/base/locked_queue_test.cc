#include "base/locked_queue.h"

#include "gtest/gtest.h"

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
    EXPECT_EQ(static_cast<unsigned>(i), queue.Size());
  }
  for (int i = 1; i < 10; ++i) {
    ASSERT_TRUE(!!(actual = queue.Pop()));
    EXPECT_EQ(i, *actual);
    EXPECT_EQ(static_cast<unsigned>(9 - i), queue.Size());
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
  TestClass expected;

  ASSERT_TRUE(queue.Push(std::move(expected)));
  LockedQueue<TestClass>::Optional&& actual = queue.Pop();
  ASSERT_TRUE(!!actual);
  EXPECT_EQ(1, actual->counter());

  queue.Close();
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
  LockedQueue<std::unique_ptr<Observer>> queue;

  std::unique_ptr<Observer> ptr(new Observer(observer_exists));
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
  // TODO: implement this.
}

}  // namespace base
}  // namespace dist_clang
