#include <base/thread.h>
#include <base/worker_pool.h>

#include <third_party/gtest/exported/include/gtest/gtest.h>

namespace dist_clang {
namespace base {

TEST(WorkerPoolTest, InstantExitOnShutdown) {
  Mutex mutex;
  std::condition_variable condition;
  bool ready = false;
  bool done = false;
  UniquePtr<WorkerPool> pool(new WorkerPool(true));

  UniqueLock lock(mutex);

  pool->AddWorker("TestWorker"_l, [&] (const WorkerPool& pool) {
    UniqueLock lock(mutex);
    condition.wait(lock, [&ready] { return ready; });
    EXPECT_TRUE(pool.WaitUntilShutdown(std::chrono::seconds(5)));
    done = true;
    condition.notify_one();
  });

  Thread thread("Test"_l, [&] { pool.reset(); });
  ready = true;
  condition.wait_for(lock, std::chrono::seconds(1), [&done] { return done; });
  ASSERT_TRUE(done);

  thread.join();
}

TEST(WorkerPoolTest, DoesNotForciblyExitOnShutdown) {
  Mutex mutex;
  std::condition_variable condition;
  bool ready = false;
  bool done = false;
  UniquePtr<WorkerPool> pool(new WorkerPool);

  UniqueLock lock(mutex);

  Clock::time_point start = Clock::now();

  pool->AddWorker("TestWorker"_l, [&] (const WorkerPool& pool) {
    UniqueLock lock(mutex);
    condition.wait(lock, [&ready] { return ready; });
    EXPECT_FALSE(pool.WaitUntilShutdown(std::chrono::seconds(1)));
    done = true;
    condition.notify_one();
  });

  Thread thread("Test"_l, [&] { pool.reset(); });
  ready = true;
  condition.wait_for(lock, std::chrono::seconds(2), [&done] { return done; });
  ASSERT_TRUE(done);
  ASSERT_LE(std::chrono::seconds(1), Clock::now() - start);

  thread.join();
}

}  // namespace base
}  // namespace dist_clang
