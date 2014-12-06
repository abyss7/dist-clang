#include <base/future.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>
#include STL(thread)

namespace dist_clang {
namespace base {

TEST(FutureTest, SimpleUsage) {
  struct Simple {
    int a = 0;
  };

  UniquePtr<Promise<Simple>> promise(new Promise<Simple>({1}));
  auto future = promise->GetFuture();

  ASSERT_TRUE(!!future);
  EXPECT_FALSE(!!(*future));
  EXPECT_EQ(0, future->GetValue().a);

  bool joined = false;
  std::thread thread([&future, &joined] {
    future->Wait();
    joined = true;
  });

  EXPECT_FALSE(joined);
  promise->SetValue({2});
  thread.join();
  EXPECT_TRUE(joined);
  EXPECT_TRUE(!!(*future));
  EXPECT_EQ(2, future->GetValue().a);

  promise.reset();
  EXPECT_EQ(2, future->GetValue().a);
}

TEST(FutureTest, DoubleSetValue) {
  Promise<int> promise(1);
  auto future = promise.GetFuture();

  promise.SetValue(2);
  EXPECT_EQ(2, future->GetValue());
  promise.SetValue(3);
  EXPECT_EQ(2, future->GetValue());
  promise.SetValue([] { return 4; });
  future->Wait();
  EXPECT_EQ(2, future->GetValue());
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

TEST(FutureTest, AsyncUsage) {
  struct Simple {
    int a = 0;
  };

  UniquePtr<Promise<Simple>> promise(new Promise<Simple>({1}));
  auto future = promise->GetFuture();

  ASSERT_TRUE(!!future);
  EXPECT_FALSE(!!(*future));
  EXPECT_EQ(0, future->GetValue().a);

  bool joined = false;
  std::thread thread([&future, &joined] {
    future->Wait();
    joined = true;
  });

  EXPECT_FALSE(joined);
  promise->SetValue([] { return Simple{2}; });
  thread.join();
  EXPECT_TRUE(joined);
  EXPECT_TRUE(!!(*future));
  EXPECT_EQ(2, future->GetValue().a);

  promise.reset();
  EXPECT_EQ(2, future->GetValue().a);
}

TEST(FutureTest, DoubleAsyncSetValue) {
  Promise<int> promise(1);
  auto future = promise.GetFuture();

  promise.SetValue([] { return 2; });
  future->Wait();
  EXPECT_EQ(2, future->GetValue());
  promise.SetValue([] { return 3; });
  future->Wait();
  EXPECT_EQ(2, future->GetValue());
  promise.SetValue(4);
  EXPECT_EQ(2, future->GetValue());
}

}  // namespace base
}  // namespace dist_clang
