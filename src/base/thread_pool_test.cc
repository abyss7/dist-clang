#include <base/thread_pool.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace base {

TEST(ThreadPoolTest, CompleteAllTasksOnDestruction) {
  Mutex mutex;
  std::condition_variable condition;
  bool ready = false;
  bool done = false;
  UniquePtr<ThreadPool> pool(new ThreadPool);

  UniqueLock lock(mutex);

  auto future = pool->Push([&] {
    UniqueLock lock(mutex);
    condition.wait(lock, [&ready] { return ready; });
    done = true;
    condition.notify_all();
  });
  pool->Run();

  Thread thread("Test"_l, [&] { pool.reset(); });
  EXPECT_TRUE(!!future);
  EXPECT_EQ(1u, pool->TaskCount());
  ready = true;
  condition.wait_for(lock, std::chrono::seconds(1), [&done] { return done; });

  if (!done) {
    thread.detach();
  } else {
    thread.join();
  }

  ASSERT_TRUE(done);
}

}  // namespace base
}  // namespace dist_clang
