#pragma once

#include <base/attributes.h>
#include <base/queue_aggregator.h>
#include <daemon/configuration.h>
#include <daemon/file_cache.h>
#include <daemon/statistic.h>
#include <gtest/gtest_prod.h>
#include <net/connection_forward.h>
#include <net/network_service.h>
#include <proto/remote.pb.h>

#include <unordered_map>

namespace dist_clang {

namespace base {
class Process;
}

namespace daemon {

FORWARD_TEST(DaemonUtilTest, ConvertFlagsFromCC2PP);
FORWARD_TEST(DaemonUtilTest, CreateProcessFromFlags);

class Daemon {
 public:
#if defined(PROFILER)
  Daemon();
#endif  // PROFILER
  ~Daemon();

  bool Initialize(const Configuration& configuration);

 private:
  FRIEND_TEST(DaemonUtilTest, ConvertFlagsFromCC2PP);
  FRIEND_TEST(DaemonUtilTest, CreateProcessFromFlags);

  using ScopedMessage = UniquePtr<proto::Universal>;
  using ScopedExecute = UniquePtr<proto::Execute>;
  using ScopedTask = std::pair<net::ConnectionPtr, ScopedExecute>;
  using Queue = base::LockedQueue<ScopedTask>;
  using QueueAggregator = base::QueueAggregator<ScopedTask>;
  using Optional = Queue::Optional;
  using CompilerMap =
      std::unordered_map<std::string /* version */, std::string /* path */>;
  using PluginNameMap =
      std::unordered_map<std::string /* name */, std::string /* path */>;
  using PluginMap =
      std::unordered_map<std::string /* version */, PluginNameMap>;

  bool SearchCache(const proto::Execute* message, FileCache::Entry* entry);
  void UpdateCacheFromFile(const proto::Execute* message,
                           const std::string& file_path,
                           const proto::Status& status);
  void UpdateCache(const proto::Execute* message, const std::string& object,
                   const proto::Status& status);

  // Convert CC to PP flags.
  static proto::Flags ConvertFlags(const proto::Flags& flags);

  bool FillFlags(proto::Flags* flags, proto::Status* status = nullptr);
  void HandleNewConnection(net::ConnectionPtr connection);
  bool HandleNewMessage(net::ConnectionPtr connection, ScopedMessage message,
                        const proto::Status& status);

  static UniquePtr<base::Process> CreateProcess(const proto::Flags& flags,
                                                ui32 uid,
                                                const std::string& cwd_path =
                                                    std::string());

  static UniquePtr<base::Process> CreateProcess(const proto::Flags& flags,
                                                const std::string& cwd_path =
                                                    std::string());

  // Workers
  void DoCheckCache(const std::atomic<bool>& is_shutting_down);
  void DoRemoteExecution(const std::atomic<bool>& is_shutting_down,
                         net::EndPointPtr end_point);
  void DoLocalExecution(const std::atomic<bool>& is_shutting_down);

  CompilerMap compilers_;
  PluginMap plugins_;
  UniquePtr<FileCache> cache_;
  UniquePtr<net::NetworkService> network_service_;
  UniquePtr<daemon::Statistic> stat_service_;
  UniquePtr<Queue> local_tasks_;
  UniquePtr<Queue> failed_tasks_;
  UniquePtr<Queue> remote_tasks_;
  UniquePtr<Queue> cache_tasks_;
  UniquePtr<QueueAggregator> all_tasks_;
  UniquePtr<base::WorkerPool> workers_;
  bool store_remote_cache_ = false;
  bool sync_cache_ = false;
};

}  // namespace daemon
}  // namespace dist_clang
