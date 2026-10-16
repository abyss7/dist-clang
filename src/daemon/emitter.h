#pragma once

#include <base/queue_aggregator.h>
#include <base/thread_pool.h>
#include <base/worker_pool.h>
#include <daemon/compilation_daemon.h>
#include <daemon/compiler_identity.h>

#include <base/test_forward.h>

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

 private:
  enum State { COMPILING, COMPILED, STORED, CANCELED };

  mutable Mutex mutex_;
  State state_ = COMPILING;
  base::Process* process_ = nullptr;
  cache::string::HandledHash handled_hash_;
};

class Emitter : public CompilationDaemon {
 public:
  explicit Emitter(const Configuration& conf);
  virtual ~Emitter();

  bool Initialize() override;

 protected:
  bool Check(const Configuration& conf) const override;
  bool Reload(const Configuration& conf) override;

 private:
  FRIEND_TEST(daemon::EmitterTest, TasksGetReshardedOnConfigurationUpdate);

  enum TaskIndex {
    CONNECTION = 0,
    MESSAGE = 1,
    SOURCE = 2,
    EXTRA_FILES = 3,
    HANDLED_HASH = 4,

    CHANGED_SHARD = 5,
    // A boolean flag to indicate if task was redistributed from it's initial
    // shard to a random one. This may happen in case of remote being down.
    // Also task shouldn't hop more than once to prevent instant hopping
    // between shards.

    PREPROCESSED_DEPS = 6,
    // The dependency file, generated along with preprocessing - for the
    // remote compilation.

    START_TIME = 7,
    // The file system time before the task started to read the inputs: the
    // inputs, modified after it, may not match the result.

    SPECULATION = 8,
    // The state of the compilation, that runs at the same time with the
    // preprocessing - if it does.
  };

  using Message = UniquePtr<base::proto::Local>;
  using Task = Tuple<net::ConnectionPtr, Message, cache::string::HandledSource,
                     cache::ExtraFiles, cache::string::HandledHash, bool,
                     Immutable, i64, SharedPtr<Speculation>>;
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
  OutputLocks direct_stores_;

  bool handle_all_tasks_ = true;
  // Indicates if we force shutdown of the remote workers pool: we shouldn't if
  // there is no coordinators, or if we stopped to poll coordinators.

  static const ui32 max_total_shards;
};

}  // namespace daemon
}  // namespace dist_clang
