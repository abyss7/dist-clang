#pragma once

#include <base/queue_aggregator.h>
#include <base/thread_pool.h>
#include <base/worker_pool.h>
#include <daemon/compilation_daemon.h>
#include <daemon/compiler_identity.h>

#include <base/test_forward.h>

#include STL(condition_variable)
#include STL(set)

namespace dist_clang {
namespace daemon {
FORWARD_TEST(EmitterTest, TasksGetReshardedOnConfigurationUpdate);

// Serializes the work of tasks with the same output: e.g. a stale task of an
// interrupted build and a new task of the next build.
class OutputLocks {
 public:
  class Lock {
   public:
    Lock(OutputLocks* locks, const String& path);
    ~Lock();

   private:
    OutputLocks* locks_;
    const String path_;
    SharedPtr<Mutex> mutex_;
  };

 private:
  friend class Lock;

  Mutex mutex_;
  HashMap<String, Pair<SharedPtr<Mutex>, ui32 /* users */>> locks_;
};

// The stores of the direct cache, that the lookups of the same key wait for.
// The stores don't wait for each other, and a lookup waits only for the stores,
// that have begun before it.
class PendingStores {
 public:
  class Store {
   public:
    Store(PendingStores* stores, const String& key);
    ~Store();

   private:
    PendingStores* stores_;
    const String key_;
    ui64 id_;
  };

  void WaitFor(const String& key) THREAD_SAFE;

 private:
  friend class Store;

  Mutex mutex_;
  std::condition_variable condition_;
  ui64 next_id_ = 0;
  HashMap<String, std::set<ui64>> stores_;
};

// The compilation, that runs at the same time with the preprocessing: if the
// preprocessed source is found in the simple cache first, the compilation is
// canceled.
class Speculation {
 public:
  // The compilation starts - returns |false| if it's canceled already.
  bool StartCompilation(base::Process* process) THREAD_SAFE;
  // The compilation ends - returns |false| if it's canceled: then its result
  // is dropped. It can't be canceled after that.
  bool FinishCompilation() THREAD_SAFE;
  // The result is stored into the cache. Sets |handled_hash| of the
  // preprocessed source, if it's known - the result is stored with it too.
  void FinishStore(cache::string::HandledHash* handled_hash) THREAD_SAFE;

  // The preprocessed source is found: cancels the compilation. Returns
  // |false| if the compilation has ended already.
  bool Cancel() THREAD_SAFE;
  // The preprocessed source isn't found: the compilation stores its result
  // with this hash too. Returns |true| if the result is stored already - then
  // the caller stores it with this hash.
  bool SetHandledHash(const cache::string::HandledHash& hash) THREAD_SAFE;

  bool canceled() const THREAD_SAFE;

  // The preprocessing starts - returns |false| if it's abandoned already. It
  // isn't canceled, when the client disconnects: it may end after the reply.
  bool StartPreprocessing(base::Process* process) THREAD_SAFE;
  void FinishPreprocessing() THREAD_SAFE;
  // The result of the compilation isn't stored: cancels the preprocessing -
  // it's useless.
  void Abandon() THREAD_SAFE;

 private:
  enum State { COMPILING, COMPILED, STORED, CANCELED };

  mutable Mutex mutex_;
  State state_ = COMPILING;
  base::Process* process_ = nullptr;
  base::Process* preprocessing_ = nullptr;
  bool abandoned_ = false;
  cache::string::HandledHash handled_hash_;
};

class Emitter : public CompilationDaemon {
 public:
  explicit Emitter(const Configuration& conf);
  virtual ~Emitter();

  bool Initialize() override;

  // What a request to stop does, after the reply - SIGTERM by default, that
  // the main thread waits for.
  inline void SetStopCallback(Fn<void()> callback) {
    stop_callback_ = std::move(callback);
  }

  // Sets the key root of the relocatable compilation: the one of the |roots|,
  // that contains its input.
  static void SetKeyRoot(
      const google::protobuf::RepeatedPtrField<String>& roots,
      const String& current_dir, base::proto::Flags* flags);

  // Sets the key of the precompiled header: it has the paths of all headers -
  // relative to its root, or absolute.
  static void SetPchKey(
      const google::protobuf::RepeatedPtrField<String>& roots,
      const String& current_dir, base::proto::Flags* flags);

 protected:
  bool Check(const Configuration& conf) const override;
  bool Reload(const Configuration& conf) override;

 private:
  FRIEND_TEST(daemon::EmitterTest, TasksGetReshardedOnConfigurationUpdate);

  using Message = UniquePtr<base::proto::Local>;
  struct Task {
    net::ConnectionPtr connection;
    Message message;
    cache::string::HandledSource source;
    cache::ExtraFiles extra_files;
    cache::string::HandledHash handled_hash;

    // If the task was redistributed from its initial shard to a random one -
    // e.g. when the remote is down. The task hops only once - not to hop
    // between the shards instantly.
    bool changed_shard = false;

    // The dependency file, generated along with the preprocessing - for the
    // remote compilation.
    Immutable preprocessed_deps;

    // The file system time before the task started to read the inputs: the
    // inputs, modified after it, may not match the result.
    i64 start_time = 0;

    // The state of the compilation, that runs at the same time with the
    // preprocessing - if it does.
    SharedPtr<Speculation> speculation;
  };
  using Queue = base::LockedQueue<Task, true>;
  using QueueAggregator = base::QueueAggregator<Task>;
  using Optional = Queue::Optional;
  using ResolveFn = Fn<net::EndPointPtr()>;

  static ui32 CalculateShard(const cache::string::HandledHash& handled_hash,
                             const ui32 total_shards);

  bool HandleNewMessage(net::ConnectionPtr connection, Universal message,
                        const net::proto::Status& status) override;

  // Fills the flags of |message| from its command line. Returns |false| if
  // the command can't be handled - with the reason in |status|.
  bool PrepareFlags(base::proto::Local* message, net::proto::Status* status);

  void SetExtraFiles(const cache::ExtraFiles& extra_files,
                     proto::Remote* message);

  void SpawnRemoteWorkers();

  void DoCheckCache(const base::WorkerPool&);
  void DoLocalExecute(const base::WorkerPool&);
  void DoRemoteExecute(const base::WorkerPool&, ResolveFn resolver, ui32 shard);
  void DoPoll(const base::WorkerPool&, Vector<ResolveFn> resolvers);

  // What to do on a miss of the direct cache.
  using Preprocessing = proto::Configuration::Emitter::Preprocessing;
  static Preprocessing GetPreprocessing(const proto::Configuration& conf);

  // Schedules the verification of the cache hit, if it's chosen for it - see
  // |verify_hits_percent|. |hit_time| is the start of the task of the hit.
  void MaybeVerifyHit(const base::proto::Local& message,
                      const cache::FileCache::Entry& entry, i64 hit_time);

  // Compiles the source of the cache hit again, and compares the results.
  void VerifyHit(base::proto::Local message, const String& handled_hash,
                 const String& object_hash, const String& dwo_hash,
                 i64 hit_time);

  Fn<void()> stop_callback_;

  UniquePtr<Queue> all_tasks_, cache_tasks_, failed_tasks_;
  UniquePtr<QueueAggregator> local_tasks_;
  UniquePtr<base::WorkerPool> workers_;
  UniquePtr<base::WorkerPool> coordinator_workers_;
  UniquePtr<base::WorkerPool> remote_workers_;

  // Runs the verifications of the cache hits one by one, and drops the new
  // ones, while too many are waiting.
  UniquePtr<base::ThreadPool> verifier_;

  // The compilations and the preprocessings at the same time with them.
  Atomic<ui32> running_processes_ = 0;

  CompilerIdentity compiler_identity_;
  OutputLocks output_locks_;
  // The sources, which results are being stored into the direct cache after
  // the reply: the lookups of them wait for it.
  PendingStores direct_stores_;

  Atomic<bool> handle_all_tasks_ = {true};
  // Indicates if we force shutdown of the remote workers pool: we shouldn't if
  // there is no coordinators, or if we stopped to poll coordinators.

  static const ui32 max_total_shards;
};

}  // namespace daemon
}  // namespace dist_clang
