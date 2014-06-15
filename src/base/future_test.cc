#include <base/future.h>

#include <third_party/gtest/public/gtest/gtest.h>
#include <third_party/libcxx/exported/include/thread>

namespace dist_clang {
namespace base {

TEST(FutureTest, SimpleUsage) {
  struct Simple {
    int a = 0;
  };

  Promise<Simple> promise({1});
  auto future = promise.GetFuture();

  ASSERT_TRUE(!!future);
  EXPECT_FALSE(!!(*future));
  EXPECT_EQ(0, future->GetValue().a);

  bool joined = false;
  std::thread thread([&future, &joined] {
    future->Wait();
    joined = true;
  });

  EXPECT_FALSE(joined);
  promise.SetValue({2});
  thread.join();
  EXPECT_TRUE(joined);
  EXPECT_TRUE(!!(*future));
  EXPECT_EQ(2, future->GetValue().a);
}

TEST(FutureTest, DoubleSetValue) {
  Promise<int> promise(1);
  auto future = promise.GetFuture();

  promise.SetValue(2);
  EXPECT_EQ(2, future->GetValue());
  promise.SetValue(3);
  EXPECT_EQ(2, future->GetValue());
}

TEST(FutureTest, BadPromise) {
  Promise<int> promise1(1);
  Promise<int> promise2(std::move(promise1));

  EXPECT_FALSE(!!promise1.GetFuture());
  EXPECT_TRUE(!!promise2.GetFuture());
}

TEST(FutureTest, FulfillOnExit) {
  UniquePtr<Future<int>> future;
  {
    Promise<int> promise(1);
    future.reset(new Future<int>(*promise.GetFuture()));
  }
  EXPECT_TRUE(!!(*future));
  EXPECT_EQ(1, future->GetValue());
}

}  // namespace base
}  // namespace dist_clang
