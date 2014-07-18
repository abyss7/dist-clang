#pragma once

#include <base/attributes.h>
#include <base/queue_aggregator.h>
#include <daemon/configuration.h>
#include <daemon/statistic.h>
#include <file_cache/file_cache.h>
#include <net/connection_forward.h>
#include <net/network_service.h>
#include <proto/remote.pb.h>

#include <third_party/gtest/public/gtest/gtest_prod.h>

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
  using ScopedTask = Pair<net::ConnectionPtr, ScopedExecute>;
  using Queue = base::LockedQueue<ScopedTask>;
  using QueueAggregator = base::QueueAggregator<ScopedTask>;
  using Optional = Queue::Optional;
  using CompilerMap = HashMap<String /* version */, String /* path */>;
  using PluginNameMap = HashMap<String /* name */, String /* path */>;
  using PluginMap = HashMap<String /* version */, PluginNameMap>;

  bool SearchCache(const proto::Execute* message, FileCache::Entry* entry);
  bool SearchDirectCache(const proto::Execute* message,
                         FileCache::Entry* entry);
  void UpdateCacheFromFlags(const proto::Execute* message,
                            const proto::Status& status);
  void UpdateCacheFromRemote(const proto::Execute* message,
                             const proto::RemoteResult& result,
                             const proto::Status& status);
  void UpdateCache(const proto::Execute* message,
                   const FileCache::Entry& entry);

  // Convert CC to PP flags.
  static proto::Flags ConvertFlags(const proto::Flags& flags);

  bool FillFlags(proto::Flags* flags, proto::Status* status = nullptr);
  void HandleNewConnection(net::ConnectionPtr connection);
  bool HandleNewMessage(net::ConnectionPtr connection, ScopedMessage message,
                        const proto::Status& status);

  static UniquePtr<base::Process> CreateProcess(
      const proto::Flags& flags, ui32 uid, const String& cwd_path = String());

  static UniquePtr<base::Process> CreateProcess(
      const proto::Flags& flags, const String& cwd_path = String());

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
  UniquePtr<proto::Configuration::Cache> cache_config_;
};

}  // namespace daemon
}  // namespace dist_clang
