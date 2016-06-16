#include <base/thread_pool.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace base {

TEST(ThreadPoolTest, CompleteAllTasksOnDestruction) {
  Mutex mutex;
  std::condition_variable condition;
  bool ready = false;

  const size_t expected_count = 200;
  Atomic<size_t> done = {0};

  UniquePtr<ThreadPool> pool(new ThreadPool);

  UniqueLock lock(mutex);

  std::vector<ThreadPool::Optional> futures;
  for (size_t i = 0; i != expected_count; ++i) {
    futures.emplace_back(pool->Push([&] {
      UniqueLock lock(mutex);
      condition.wait(lock, [&ready] { return ready; });
      ++done;
      condition.notify_all();
    }));
  }
  pool->Run();

  EXPECT_EQ(expected_count, pool->TaskCount());

  for (size_t i = 0; i != expected_count; ++i) {
    EXPECT_TRUE(!!futures[i]);
  }

  Thread thread("Test"_l, [&] { pool.reset(); });

  ready = true;
  lock.unlock();
  condition.notify_one();

  thread.join();
  ASSERT_EQ(expected_count, done);
}

}  // namespace base
}  // namespace dist_clang
