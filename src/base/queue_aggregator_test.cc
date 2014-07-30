#include <base/queue_aggregator.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace base {

TEST(QueueAggregatorTest, UniquePtrFriendliness) {
  class Observer {
   public:
    Observer(bool& exist) : exist_(exist) { exist_ = true; }
    ~Observer() { exist_ = false; }

   private:
    bool& exist_;
  };

  QueueAggregator<UniquePtr<Observer>> aggregator;
  LockedQueue<UniquePtr<Observer>> queue;
  aggregator.Aggregate(&queue);

  {
    bool observer_exists = true;
    UniquePtr<Observer> ptr(new Observer(observer_exists));

    ASSERT_TRUE(queue.Push(std::move(ptr)));
    EXPECT_FALSE(ptr);
    EXPECT_TRUE(observer_exists);

    auto&& actual = aggregator.Pop();
    EXPECT_TRUE(observer_exists);
    ASSERT_TRUE(!!actual);
    ASSERT_TRUE(!!(*actual));
  }

  {
    bool observer_exists = true;
    UniquePtr<Observer> ptr(new Observer(observer_exists));

    ASSERT_TRUE(queue.Push(std::move(ptr)));
    EXPECT_FALSE(ptr);
    EXPECT_TRUE(observer_exists);

    queue.Close();
    aggregator.Close();

    auto&& actual = aggregator.Pop();
    EXPECT_TRUE(observer_exists);
    ASSERT_TRUE(!!actual);
    ASSERT_TRUE(!!(*actual));
  }
}

TEST(QueueAggregatorTest, SharedPtrFriendliness) {
  QueueAggregator<std::shared_ptr<int>> aggregator;
  LockedQueue<std::shared_ptr<int>> queue;
  std::shared_ptr<int> ptr(new int);
  aggregator.Aggregate(&queue);

  ASSERT_TRUE(queue.Push(ptr));
  EXPECT_EQ(2, ptr.use_count());

  {
    auto&& actual = aggregator.Pop();
    ASSERT_TRUE(!!actual);
    EXPECT_EQ(2, actual->use_count());
  }

  ASSERT_TRUE(queue.Push(ptr));
  EXPECT_EQ(2, ptr.use_count());

  queue.Close();
  aggregator.Close();

  {
    auto&& actual = aggregator.Pop();
    ASSERT_TRUE(!!actual);
    EXPECT_EQ(2, actual->use_count());
  }
}
}
}
