#include <daemon/emitter.h>

#include <base/file/file.h>
#include <base/file_utils.h>
#include <base/logging.h>
#include <base/platform.h>
#include <base/process.h>
#include <base/temporary_dir.h>
#include <daemon/driver_args.h>
#include <daemon/outputs.h>
#include <net/connection.h>
#include <net/end_point.h>
#include <perf/counter.h>
#include <perf/stat_reporter.h>
#include <perf/stat_service.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <base/using_log.h>

#include STL(random)

using namespace std::placeholders;

namespace dist_clang {

using perf::proto::Metric;

template <bool ReportByDefault = true>
using Counter = perf::Counter<perf::StatReporter, ReportByDefault>;

namespace {

// Select a new shard, different from current.
inline ui32 FindNewShard(const ui32 total_shards, const ui32 current_shard) {
  thread_local static std::random_device random_device;
  std::uniform_int_distribution<ui32> distribution(0, total_shards - 2);
  const ui32 new_shard = distribution(random_device);
  if (new_shard >= current_shard) {
    return new_shard + 1;
  }
  return new_shard;
}

inline String GetOutputPath(const base::proto::Local* WEAK_PTR message) {
  DCHECK(message);
  if (message->flags().output()[0] == '/') {
    return message->flags().output();
  } else {
    return message->current_dir() + "/" + message->flags().output();
  }
}

// The same source in other checkouts has the same direct key: the stores of
// the sources with the same name are waited for - it's cheap.
inline String DirectStoreKey(const base::proto::Local* WEAK_PTR message) {
  DCHECK(message);
  return base::PathKey(Path(message->flags().input()).filename().string());
}

// The key of the lock of the output: the same for its spellings.
inline String OutputLockKey(const base::proto::Local* WEAK_PTR message) {
  return base::PathKey(
      Path(GetOutputPath(message)).lexically_normal().string());
}

inline String GetDepsPath(const base::proto::Local* WEAK_PTR message) {
  DCHECK(message);
  if (message->flags().deps_file()[0] == '/') {
    return message->flags().deps_file();
  } else {
    return message->current_dir() + "/" + message->flags().deps_file();
  }
}

// Checks if the client asks not to touch unchanged outputs.
inline bool RequestsRestat(const base::proto::Local* WEAK_PTR message) {
  return std::find(message->env().begin(), message->env().end(),
                   "DC_RESTAT=1") != message->env().end();
}

// Checks if the trace of the time is given with the result - from the cache
// too.
inline bool RestoresTimeTrace(const base::proto::Local* WEAK_PTR message,
                              const daemon::proto::Configuration& conf) {
  return message->flags().time_trace() && conf.cache().store_time_traces();
}

// Checks if the result is needed from the compilation itself: the cache is
// only updated. The client asks for it e.g. to measure the build.
inline bool SkipsLookup(const base::proto::Local* WEAK_PTR message,
                        const daemon::proto::Configuration& conf) {
  return (message->flags().time_trace() && !RestoresTimeTrace(message, conf)) ||
         std::find(message->env().begin(), message->env().end(),
                   "DC_RECACHE=1") != message->env().end();
}

inline String GetTimeTracePath(const base::proto::Local* WEAK_PTR message) {
  DCHECK(message);
  if (message->flags().time_trace_file()[0] == '/') {
    return message->flags().time_trace_file();
  } else {
    return message->current_dir() + "/" + message->flags().time_trace_file();
  }
}

// The number of the links of the cache file outside of the cache, except the
// |path|, if it's one of them.
ui64 OtherLinks(const Path& cache_file, const Path& path) {
  struct stat cache_st, path_st;
  if (stat(cache_file.c_str(), &cache_st) == -1) {
    return 0;
  }
  const bool same_file = stat(path.c_str(), &path_st) == 0 &&
                         path_st.st_ino == cache_st.st_ino &&
                         path_st.st_dev == cache_st.st_dev;
  return cache_st.st_nlink - 1 - (same_file ? 1 : 0);
}

// Puts the output from the cache to |path|: links the cache file, if possible.
// With |keep_unchanged| doesn't touch the output, if it's the same already.
// The cache file is hard-linked to at most |max_links| places (0 - to any
// number): the build systems don't expect, that a modification of their
// output in place changes the outputs of other builds, and its modification
// time too.
bool RestoreOutput(const Path& cache_file, const String& cache_hash,
                   Immutable content, const String& path, bool keep_unchanged,
                   ui32 max_links, const base::proto::Local* WEAK_PTR message,
                   String* error) {
  if (keep_unchanged &&
      (cache_file.empty() ? daemon::HasSameContents(path, content)
                          : daemon::HasSameContents(path, cache_file,
                                                    cache_hash))) {
    STAT(CACHE_OUTPUT_UNCHANGED);
    STAT(OUTPUT_WRITES_AVOIDED, cache_file.empty()
                                    ? content.size()
                                    : base::File::Size(cache_file));
    return true;
  }

  // The hard-linked file can't have another owner.
  const bool same_user =
      !message->has_user_id() || message->user_id() == getuid();

  if (!cache_file.empty()) {
    base::File::CloneMode mode;
    const bool allow_hardlink =
        same_user && (!max_links || OtherLinks(cache_file, path) < max_links);
    if (!base::File::Clone(cache_file, path, allow_hardlink, &mode, error)) {
      return false;
    }
    // Another task may have linked the same file meanwhile.
    if (mode == base::File::CloneMode::HARDLINK && max_links &&
        OtherLinks(cache_file, path) >= max_links &&
        !base::File::Clone(cache_file, path, false, &mode, error)) {
      return false;
    }
    // The restored file must look newer than the sources.
    utimensat(AT_FDCWD, path.c_str(), nullptr, 0);
    if (mode == base::File::CloneMode::COPY) {
      STAT(CACHE_OUTPUT_COPIED);
    } else {
      STAT(CACHE_OUTPUT_LINKED);
      STAT(OUTPUT_WRITES_AVOIDED, base::File::Size(cache_file));
    }
  } else {
    if (!base::File::Write(path, content, error)) {
      return false;
    }
    STAT(CACHE_OUTPUT_COPIED);
  }

  if (!same_user) {
    String chown_error;
    if (!base::ChangeOwner(path, message->user_id(), &chown_error)) {
      LOG(ERROR) << "Failed to change owner for " << path << " : "
                 << chown_error;
    }
  }

  return true;
}

inline String GetDwoPath(const base::proto::Local* WEAK_PTR message) {
  DCHECK(message);
  if (message->flags().dwo_file()[0] == '/') {
    return message->flags().dwo_file();
  } else {
    return message->current_dir() + "/" + message->flags().dwo_file();
  }
}

// Checks if the dependency file target is set explicitly via flags.
inline bool HasExplicitDepsTarget(const base::proto::Flags& flags) {
  for (const auto& arg : flags.non_direct()) {
    if (arg.values_size() && (arg.values(0).starts_with("-MT") ||
                              arg.values(0).starts_with("-MQ"))) {
      return true;
    }
  }
  return false;
}

// Some compilations can't be done remotely: they depend on local files, which
// aren't sent, or produce more than one output file.
inline bool IsRemotable(const base::proto::Flags& flags) {
  return !flags.has_dwo_file() && !flags.key_files_size();
}

// Makes the compilation write the dependency file into |path|.
void RedirectDepsFile(base::proto::Flags* flags, const Path& path) {
  auto* args = flags->mutable_non_direct();
  for (int i = 0; i < args->size();) {
    if (args->Get(i).values_size() && args->Get(i).values(0).starts_with("-MF")) {
      args->DeleteSubrange(i, 1);
    } else {
      ++i;
    }
  }
  auto* deps_file = flags->add_non_direct();
  deps_file->set_index(0);
  deps_file->add_values("-MF");
  deps_file->add_values(path.string());
}

// Preprocesses the source. The dependency file is generated along with
// preprocessing - for the case of the remote compilation - but never into the
// build directory: a preprocessing, which isn't needed anymore, mustn't
// overwrite the real one.
inline bool GenerateSource(const base::proto::Local* WEAK_PTR message,
                           const net::ConnectionPtr& connection,
                           cache::string::HandledSource* source,
                           Immutable* deps,
                           daemon::Speculation* speculation = nullptr) {
  Counter<> preprocess_time_counter(Metric::PREPROCESS_TIME);
  STAT(PREPROCESSINGS);
  base::proto::Flags pp_flags;

  DCHECK(message);
  pp_flags.CopyFrom(message->flags());
  pp_flags.clear_cc_only();
  pp_flags.clear_ignored();
  pp_flags.set_output("-");
  pp_flags.set_action("-E");

  std::optional<base::TemporaryDir> deps_dir;
  if (pp_flags.has_deps_file()) {
    deps_dir.emplace();
    RedirectDepsFile(&pp_flags, deps_dir->path() / "deps.d");

    // The default target depends on the output, so set it explicitly.
    if (pp_flags.has_deps_target() && !HasExplicitDepsTarget(pp_flags)) {
      auto* target = pp_flags.add_non_direct();
      target->set_index(0);
      target->add_values("-MT");
      target->add_values(pp_flags.deps_target());
    }
  }

  // Clang plugins can't affect source code.
  pp_flags.mutable_compiler()->clear_plugins();

  // Sanitizer blacklist can't affect source code
  pp_flags.clear_sanitize_blacklist();

  base::ProcessPtr process;
  if (message->has_user_id()) {
    process = daemon::CompilationDaemon::CreateProcess(
        pp_flags, message->user_id(), Path(message->current_dir()));
  } else {
    process = daemon::CompilationDaemon::CreateProcess(
        pp_flags, Path(message->current_dir()));
  }
  if (!speculation) {
    process->CancelOnDisconnect(connection->NativeHandle());
  } else if (!speculation->StartPreprocessing(process.get())) {
    return false;
  }

  const bool preprocessed = process->Run(base::Process::UNLIMITED);
  if (speculation) {
    speculation->FinishPreprocessing();
  }
  if (!preprocessed) {
    return false;
  }

  if (source) {
    source->str.assign(process->stdout());
  }
  if (deps && deps_dir) {
    base::File::Read(deps_dir->path() / "deps.d", deps);
  }

  return true;
}

}  // namespace

namespace daemon {

bool Speculation::StartCompilation(base::Process* process) {
  std::lock_guard<Mutex> lock(mutex_);
  if (state_ == CANCELED) {
    return false;
  }
  process_ = process;
  return true;
}

bool Speculation::FinishCompilation() {
  std::lock_guard<Mutex> lock(mutex_);
  process_ = nullptr;
  if (state_ == CANCELED) {
    return false;
  }
  state_ = COMPILED;
  return true;
}

void Speculation::FinishStore(cache::string::HandledHash* handled_hash) {
  std::lock_guard<Mutex> lock(mutex_);
  state_ = STORED;
  *handled_hash = handled_hash_;
}

bool Speculation::Cancel() {
  std::lock_guard<Mutex> lock(mutex_);
  if (state_ != COMPILING) {
    return false;
  }
  state_ = CANCELED;
  if (process_) {
    process_->Cancel();
  }
  return true;
}

bool Speculation::SetHandledHash(const cache::string::HandledHash& hash) {
  std::lock_guard<Mutex> lock(mutex_);
  handled_hash_ = hash;
  return state_ == STORED;
}

bool Speculation::canceled() const {
  std::lock_guard<Mutex> lock(mutex_);
  return state_ == CANCELED;
}

bool Speculation::StartPreprocessing(base::Process* process) {
  std::lock_guard<Mutex> lock(mutex_);
  if (abandoned_) {
    return false;
  }
  preprocessing_ = process;
  return true;
}

void Speculation::FinishPreprocessing() {
  std::lock_guard<Mutex> lock(mutex_);
  preprocessing_ = nullptr;
}

void Speculation::Abandon() {
  std::lock_guard<Mutex> lock(mutex_);
  // The result is stored, or found by the preprocessing.
  if (state_ == STORED || state_ == CANCELED) {
    return;
  }
  abandoned_ = true;
  if (preprocessing_) {
    preprocessing_->Cancel();
  }
}

OutputLocks::Lock::Lock(OutputLocks* locks, const String& path)
    : locks_(locks), path_(path) {
  {
    std::lock_guard<Mutex> guard(locks_->mutex_);
    auto& lock = locks_->locks_[path_];
    if (!lock.first) {
      lock.first = std::make_shared<Mutex>();
    }
    ++lock.second;
    mutex_ = lock.first;
  }
  mutex_->lock();
}

OutputLocks::Lock::~Lock() {
  mutex_->unlock();
  std::lock_guard<Mutex> guard(locks_->mutex_);
  auto it = locks_->locks_.find(path_);
  if (--it->second.second == 0) {
    locks_->locks_.erase(it);
  }
}

PendingStores::Store::Store(PendingStores* stores, const String& key)
    : stores_(stores), key_(key) {
  std::lock_guard<Mutex> guard(stores_->mutex_);
  id_ = stores_->next_id_++;
  stores_->stores_[key_].insert(id_);
}

PendingStores::Store::~Store() {
  {
    std::lock_guard<Mutex> guard(stores_->mutex_);
    auto it = stores_->stores_.find(key_);
    it->second.erase(id_);
    if (it->second.empty()) {
      stores_->stores_.erase(it);
    }
  }
  stores_->condition_.notify_all();
}

void PendingStores::WaitFor(const String& key) {
  UniqueLock lock(mutex_);
  const ui64 id = next_id_;
  condition_.wait(lock, [&] {
    auto it = stores_.find(key);
    return it == stores_.end() || *it->second.begin() >= id;
  });
}

const ui32 Emitter::max_total_shards = 1024u;

Emitter::Emitter(const proto::Configuration& conf)
    : CompilationDaemon(conf), stop_callback_([] { kill(getpid(), SIGTERM); }) {
  using Worker = base::WorkerPool::SimpleWorker;

  CHECK(conf.has_emitter());

  workers_ = std::make_unique<base::WorkerPool>();
  verifier_ = std::make_unique<base::ThreadPool>(16, 1);
  coordinator_workers_ = std::make_unique<base::WorkerPool>(true);
  all_tasks_ = std::make_unique<Queue>(Seconds(conf.emitter().pop_timeout()));
  cache_tasks_ = std::make_unique<Queue>();
  failed_tasks_ = std::make_unique<Queue>();

  local_tasks_ = std::make_unique<QueueAggregator>();
  local_tasks_->Aggregate(failed_tasks_.get());
  if (!conf.emitter().only_failed()) {
    local_tasks_->Aggregate(all_tasks_.get());
  }

  {
    Worker worker = std::bind(&Emitter::DoLocalExecute, this, _1);
    workers_->AddWorker("Local Execute"_l, worker,
                        conf.emitter().threads());
  }

  if (conf.has_cache() && !conf.cache().disabled()) {
    Worker worker = std::bind(&Emitter::DoCheckCache, this, _1);
    if (conf.cache().has_threads()) {
      workers_->AddWorker("Cache Worker"_l, worker, conf.cache().threads());
    } else {
      workers_->AddWorker("Cache Worker"_l, worker,
                          std::thread::hardware_concurrency());
    }
  }

  {
    Vector<ResolveFn> resolvers;
    for (const auto& coordinator : conf.emitter().coordinators()) {
      if (coordinator.disabled()) {
        continue;
      }
      resolvers.push_back([
        this, host = coordinator.host(),
        port = static_cast<ui16>(coordinator.port()), ipv6 = coordinator.ipv6()
      ]() {
        auto optional = resolver_->Resolve(host, port, ipv6);
        DCHECK(optional);
        optional->Wait();
        return optional->GetValue();
      });
    }

    // FIXME(ilezhankin): it's nicer to have a worker per coordinator, but I
    //                    don't know how to organize the fallback policy.
    if (!resolvers.empty()) {
      handle_all_tasks_ = false;
      Worker worker = std::bind(&Emitter::DoPoll, this, _1, resolvers);
      coordinator_workers_->AddWorker("Coordinator"_l, worker);
    }
  }
}

Emitter::~Emitter() {
  all_tasks_->Close();
  cache_tasks_->Close();
  failed_tasks_->Close();
  local_tasks_->Close();
  coordinator_workers_.reset();
  workers_.reset();
  remote_workers_.reset();
  verifier_.reset();
}

bool Emitter::Initialize() {
  auto conf = this->conf();

  String error;
  if (!Listen(conf->emitter().socket_path(), &error)) {
    LOG(ERROR) << "Emitter failed to listen on "
               << conf->emitter().socket_path() << " : " << error;
    return false;
  }

  verifier_->Run();
  return CompilationDaemon::Initialize();
}

// static
Emitter::Preprocessing Emitter::GetPreprocessing(
    const proto::Configuration& conf) {
  // The remote compilation needs the preprocessed source.
  if (conf.emitter().remotes_size() || conf.emitter().coordinators_size()) {
    return proto::Configuration::Emitter::ALWAYS;
  }
  return conf.emitter().preprocessing();
}

void Emitter::MaybeVerifyHit(const base::proto::Local& message,
                             const cache::FileCache::Entry& entry,
                             i64 hit_time) {
  const ui32 percent = conf()->emitter().verify_hits_percent();
  if (!percent || entry.handled_hash.empty() || entry.object_hash.empty()) {
    return;
  }
  // The command line in the debug info has the path of the output, which is
  // another one for the verification.
  if (message.flags().records_command_line()) {
    return;
  }
  // The precompiled header from another root has this root in it.
  if (message.flags().pch()) {
    return;
  }

  thread_local std::mt19937 generator(std::random_device{}());
  if (std::uniform_int_distribution<ui32>(0, 99)(generator) >= percent) {
    return;
  }

  // Don't let the verifications pile up.
  verifier_->Push([this, message, handled_hash = entry.handled_hash,
                   object_hash = entry.object_hash,
                   dwo_hash = entry.dwo_hash, hit_time] {
    VerifyHit(message, handled_hash, object_hash, dwo_hash, hit_time);
  });
}

void Emitter::VerifyHit(base::proto::Local message, const String& handled_hash,
                        const String& object_hash, const String& dwo_hash,
                        i64 hit_time) {
  auto* flags = message.mutable_flags();
  if (!SetupCompiler(flags, nullptr)) {
    return;
  }

  // Nothing is written into the build directory - unless the restored result
  // is wrong.
  const String restored_output = GetOutputPath(&message);
  const String restored_dwo =
      flags->has_dwo_file() ? GetDwoPath(&message) : String();
  const String output_lock_key = OutputLockKey(&message);
  const base::TemporaryDir temp_dir;
  const Path output_path = temp_dir.path() / "output.o";
  const Path dwo_path = temp_dir.path() / "output.dwo";
  const Path deps_path = temp_dir.path() / "deps.d";
  flags->clear_ignored();
  flags->set_output(output_path.string());
  // The headers are known from the dependency file: their edits after the hit
  // give another result too.
  if (!flags->has_deps_file()) {
    auto* arg = flags->add_non_direct();
    arg->set_index(0);
    arg->add_values("-MD");
  }
  RedirectDepsFile(flags, deps_path);
  if (flags->has_dwo_file()) {
    // The object refers to the split DWARF file by its original name.
    auto* arg = flags->add_other();
    arg->set_index(std::numeric_limits<ui32>::max());
    const List<String> values = {
        "-Xclang", "-split-dwarf-file",   "-Xclang", flags->dwo_file(),
        "-Xclang", "-split-dwarf-output", "-Xclang", dwo_path.string()};
    for (const auto& value : values) {
      arg->add_values(value);
    }
  }

  const ui32 uid =
      message.has_user_id() ? message.user_id() : base::Process::SAME_UID;
  auto process = CreateProcess(*flags, uid, Path(message.current_dir()));
  process->SetNice(19);
  String error;
  if (!process->Run(base::Process::UNLIMITED, &error)) {
    LOG(WARNING) << "Failed to verify the cache hit for " << flags->input()
                 << ": " << (error.empty() ? process->stderr().string_copy()
                                           : error);
    return;
  }

  // The inputs, changed since the hit, give another result.
  Immutable deps;
  if (!base::File::Read(deps_path, &deps) ||
      !InputsUnchangedSince(&message, deps, hit_time)) {
    return;
  }

  auto SameHash = [](const Path& path, const String& expected_hash) {
    Immutable hash;
    return base::File::Hash(path, &hash) && hash.string_copy() == expected_hash;
  };
  const bool has_dwo = flags->has_dwo_file() && base::File::Exists(dwo_path);
  if (SameHash(output_path, object_hash) &&
      has_dwo == !dwo_hash.empty() &&
      (!has_dwo || SameHash(dwo_path, dwo_hash))) {
    STAT(HIT_VERIFIED);
    return;
  }

  LOG(ERROR) << "The cached result differs from the compilation of "
             << flags->input() << " in " << message.current_dir()
             << " - the cache entry " << handled_hash << " is removed";

  // The build system takes the restored result for the right one - until the
  // source is edited: it's replaced with the verified one, unless it's
  // changed since. The new file doesn't change the linked copy in the cache.
  {
    OutputLocks::Lock output_lock(&output_locks_, output_lock_key);
    auto Replace = [&](const Path& from, const String& to,
                       const String& restored_hash) {
      Immutable content;
      String error;
      if (!SameHash(to, restored_hash) || !base::File::Read(from, &content) ||
          !base::File::Write(to, content, &error) ||
          (message.has_user_id() &&
           !base::ChangeOwner(to, message.user_id(), &error))) {
        if (!error.empty()) {
          LOG(WARNING) << "Failed to replace the wrong result " << to << ": "
                       << error;
        }
        return false;
      }
      return true;
    };
    if (Replace(output_path, restored_output, object_hash) && has_dwo) {
      Replace(dwo_path, restored_dwo, dwo_hash);
    }
  }

  STAT(HIT_VERIFICATION_FAILED);
  RemoveFromCache(cache::string::HandledHash(Immutable(handled_hash)));
}

// static
ui32 Emitter::CalculateShard(const cache::string::HandledHash& handled_hash,
                             const ui32 total_shards) {
  return (*reinterpret_cast<const ui32*>(handled_hash.str.Hash(4).c_str())) %
         total_shards;
}

bool Emitter::HandleNewMessage(net::ConnectionPtr connection, Universal message,
                               const net::proto::Status& status) {
  using namespace cache::string;

  auto conf = this->conf();
  if (!message->IsInitialized()) {
    LOG(INFO) << message->InitializationErrorString();
    return false;
  }

  if (status.code() != net::proto::Status::OK) {
    LOG(ERROR) << status.description();
    return connection->ReportStatus(status);
  }

  // The socket may be reachable by other users - e.g. in "/tmp".
  if (const int fd = connection->NativeHandle(); fd != -1) {
    ui32 peer_uid = 0;
    String reason = "Can't get the user of the client";
    if (!net::Connection::SocketPeerUid(fd, &peer_uid) ||
        !MayServe(getuid(), peer_uid, *message, &reason)) {
      LOG(WARNING) << reason;
      net::proto::Status status;
      status.set_code(net::proto::Status::BAD_MESSAGE);
      status.set_description(reason);
      return connection->ReportStatus(status);
    }
  }

  if (message->has_local()) {
    Message execute(message->release_local());

    net::proto::Status status;
    if (!PrepareFlags(execute.get(), &status)) {
      STAT(UNSUPPORTED_COMMAND);
      return connection->ReportStatus(status);
    }

    Task task{.connection = connection,
              .message = std::move(execute),
              .start_time = FileSystemTime()};
    if (conf->has_cache() && !conf->cache().disabled()) {
      return cache_tasks_->Push(std::move(task));
    } else if (!IsRemotable(task.message->flags())) {
      return failed_tasks_->Push(std::move(task));
    } else {
      return all_tasks_->Push(std::move(task));
    }
  }

  if (message->has_tags_request()) {
    return HandleTagsRequest(
        connection, message->tags_request());
  }

  if (message->has_stop_request()) {
    LOG(INFO) << "Stopping by the request of the client";
    net::proto::Status status;
    status.set_code(net::proto::Status::OK);
    return connection->ReportStatus(
        status, [this](net::ConnectionPtr connection,
                       const net::proto::Status& status) {
          const bool result =
              net::Connection::CloseAfterSend()(connection, status);
          stop_callback_();
          return result;
        });
  }

  if (message->has_report()) {
    auto report = std::make_unique<perf::proto::Report>();
    base::Singleton<perf::StatService>::Get().DumpAll(report.get());
    DaemonStatus(report->mutable_daemon());
    return connection->SendAsync(std::move(report));
  }

  NOTREACHED();
  return false;
}

bool Emitter::PrepareFlags(base::proto::Local* message,
                           net::proto::Status* status) {
  DCHECK(message);
  DCHECK(status);

  // The flags may be already filled - e.g. by tests.
  if (!message->args_size()) {
    if (message->has_flags()) {
      return true;
    }
    status->set_code(net::proto::Status::BAD_MESSAGE);
    status->set_description("No command line");
    return false;
  }

  auto Unsupported = [&](const String& reason) {
    LOG(VERBOSE) << "Unsupported command: " << reason;
    status->set_code(net::proto::Status::UNSUPPORTED);
    status->set_description(reason);
    return false;
  };

  const String& current_dir = message->current_dir();
  List<String> args(std::next(message->args().begin()), message->args().end());
  const List<String> env(message->env().begin(), message->env().end());

  String error;
  if (!ExpandResponseFiles(args, current_dir, &error)) {
    return Unsupported("Failed to expand response files: " + error);
  }

  auto* flags = message->mutable_flags();
  flags->Clear();

  String reason;
  if (!ParseDriverArgs(args, current_dir, flags, &reason) ||
      !ParseEnvironment(env, flags, &reason)) {
    return Unsupported(reason);
  }

  // E.g. the output into "/dev/null" isn't a result to cache, and the input
  // from "/dev/stdin" is the one of the daemon.
  String special_path;
  if (FindSpecialFile(*flags, current_dir, &special_path)) {
    return Unsupported("not a regular file: " + special_path);
  }

  const String compiler_path =
      CompilerIdentity::Resolve(message->args(0), current_dir, env);
  if (compiler_path.empty()) {
    return Unsupported("Can't find compiler " + message->args(0));
  }

  String identity;
  if (!compiler_identity_.Get(compiler_path, &identity, &error)) {
    return Unsupported("Can't identify compiler " + compiler_path + ": " +
                       error);
  }

  flags->mutable_compiler()->set_path(compiler_path);
  flags->mutable_compiler()->set_version(identity);

  // A new file in an include directory may shadow a header from the own
  // directories of the compiler too.
  List<String> include_dirs;
  if (compiler_identity_.GetIncludeDirs(compiler_path, identity, *flags,
                                        &include_dirs, &error)) {
    for (auto& dir : include_dirs) {
      flags->add_implicit_include_dirs(std::move(dir));
    }
  } else {
    LOG(WARNING) << error;
  }
  flags->set_recheck_warnings(conf()->emitter().recheck_warnings());

  // The same sources from different places should share the cache.
  if (flags->pch()) {
    SetPchKey(conf()->cache().source_roots(), current_dir, flags);
  } else if (flags->relocatable()) {
    SetKeyRoot(conf()->cache().source_roots(), current_dir, flags);
  }

  return true;
}

// static
void Emitter::SetKeyRoot(
    const google::protobuf::RepeatedPtrField<String>& roots,
    const String& current_dir, base::proto::Flags* flags) {
  const String input =
      (Path(current_dir) / flags->input()).lexically_normal().string();
  const String input_key = base::PathKey(input);
  for (const auto& root : roots) {
    String root_path = Path(root).lexically_normal().string();
    while (root_path.size() > 1 && root_path.back() == '/') {
      root_path.pop_back();
    }
    if (input_key.starts_with(base::PathKey(root_path) + "/")) {
      // The root is replaced in the paths, that the compiler sees - in their
      // spelling.
      const String key_root = input.substr(0, root_path.size());
      const String key_root_relative =
          Path(key_root).lexically_relative(current_dir).string();
      // Only the forms of the root, that the compiler removes from the paths
      // of the result, may be replaced in the key: otherwise the objects of
      // different roots - or of the build directories at different depths -
      // would be mixed.
      auto Remapped = [&flags](const String& path) {
        for (const auto& prefix : flags->remapped_prefixes()) {
          if (base::PathKey(prefix) == base::PathKey(path)) {
            return true;
          }
        }
        return false;
      };
      const bool remapped = Remapped(key_root);
      // The paths relative to the root itself don't contain it.
      const bool relative_remapped =
          key_root_relative == "." || Remapped(key_root_relative);
      if (!remapped && !relative_remapped) {
        return;
      }
      flags->set_key_root(key_root);
      flags->set_key_root_relative(key_root_relative);
      flags->set_key_root_remapped(remapped);
      flags->set_key_root_relative_remapped(relative_remapped &&
                                            key_root_relative != ".");
      return;
    }
  }
}

// static
void Emitter::SetPchKey(
    const google::protobuf::RepeatedPtrField<String>& roots,
    const String& current_dir, base::proto::Flags* flags) {
  // The compiler makes the paths relative only to the root, that is given
  // literally, and only the paths inside it: the build directory too.
  if (flags->relocatable() && flags->has_pch_sysroot()) {
    SetKeyRoot(roots, current_dir, flags);
    String sysroot = flags->pch_sysroot();
    while (sysroot.size() > 1 && sysroot.back() == '/') {
      sysroot.pop_back();
    }
    const String build_dir =
        Path(current_dir).lexically_relative(flags->key_root()).string();
    if (flags->has_key_root() && sysroot == flags->key_root() &&
        !build_dir.empty() && build_dir != ".." &&
        !build_dir.starts_with("../")) {
      flags->add_key_extra("pch-cwd:" + build_dir);
      return;
    }
    flags->clear_key_root();
    flags->clear_key_root_relative();
    flags->clear_key_root_remapped();
    flags->clear_key_root_relative_remapped();
  }
  flags->add_key_extra("pch-cwd:" + current_dir);
}

void Emitter::SetExtraFiles(const cache::ExtraFiles& extra_files,
                            proto::Remote* message) {
  DCHECK(message);

  auto sanitize_blacklist = extra_files.find(cache::SANITIZE_BLACKLIST);
  if (sanitize_blacklist != extra_files.end()) {
    message->set_sanitize_blacklist(sanitize_blacklist->second);
  }
}

void Emitter::DoCheckCache(const base::WorkerPool& pool) {
  using namespace cache::string;

  while (!pool.IsShuttingDown()) {
    auto conf = this->conf();

    Optional&& task = cache_tasks_->Pop();
    if (!task) {
      break;
    }

    if (task->connection->IsPeerGone()) {
      STAT(TASK_CANCELED);
      continue;
    }

    base::proto::Local* incoming = task->message.get();
    cache::FileCache::Entry entry;
    // The build of an isolated tag finds only its own entries.
    const String scope = IsolatedTag(*incoming);

    // Returns |true|, if the task is handled - the outputs are restored, or the
    // client has got the failure, or is gone: only the restored ones are hits.
    bool restored = false;
    auto RestoreFromCache = [&](const HandledSource& source,
                                const cache::ExtraFiles& extra_files) {
      String error;
      const String output_path = GetOutputPath(incoming);
      const auto& flags = incoming->flags();

      // Only one task works with the output at a time. Then a stale task can
      // only touch it before the task of the waiting client.
      OutputLocks::Lock output_lock(&output_locks_, OutputLockKey(incoming));

      // Nobody waits for the outputs - don't write them.
      if (task->connection->IsPeerGone()) {
        STAT(TASK_CANCELED);
        return true;
      }

      // The dependency file isn't a part of the key - the entry may be
      // created by a compilation without it.
      if (flags.has_deps_file() && entry.deps.empty()) {
        LOG(CACHE_INFO) << "Cache entry has no dependency file: "
                        << flags.input();
        return false;
      }

      // Only the entry with the trace of the same options gives it.
      Immutable time_trace;
      if (RestoresTimeTrace(incoming, *conf) &&
          (entry.time_trace_options != TimeTraceOptions(flags) ||
           !SearchTimeTrace(entry, &time_trace))) {
        LOG(CACHE_INFO) << "Cache entry has no time trace: " << flags.input();
        return false;
      }

      // The cached diagnostics are produced with other warnings, or in another
      // format - check the source again, but without the code generation.
      String diagnostics = entry.stderr.string_copy();
      if (entry.warnings_hash != WarningsHash(flags) &&
          (flags.recheck_warnings() || !entry.stderr.empty())) {
        net::proto::Status setup_status;
        if (!SetupCompiler(incoming->mutable_flags(), &setup_status)) {
          return false;
        }
        // Keep the flags of the diagnostics format, but not of the dependency
        // file: nothing is written into the build directory, until the
        // outputs are restored.
        auto check_flags = flags;
        check_flags.set_action("-fsyntax-only");
        check_flags.clear_output();
        check_flags.clear_ignored();
        auto* args = check_flags.mutable_non_direct();
        for (int i = 0; i < args->size();) {
          if (args->Get(i).values_size() &&
              args->Get(i).values(0).starts_with("-M")) {
            args->DeleteSubrange(i, 1);
          } else {
            ++i;
          }
        }
        const ui32 uid = incoming->has_user_id() ? incoming->user_id()
                                                 : base::Process::SAME_UID;
        auto process =
            CreateProcess(check_flags, uid, Path(incoming->current_dir()));
        process->CancelOnDisconnect(
            task->connection->NativeHandle());
        STAT(WARNINGS_RECHECKED);
        if (!process->Run(base::Process::UNLIMITED, &error)) {
          // The check itself has failed - the compilation does it instead.
          if (!process->exited()) {
            return false;
          }
          // The same errors, as the compilation would report - e.g. with
          // "-Werror".
          net::proto::Status status;
          status.set_code(net::proto::Status::EXECUTION);
          status.set_description(process->stderr().empty()
                                     ? error
                                     : process->stderr().string_copy());
          task->connection->ReportStatus(status);
          LOG(INFO) << "Recheck of warnings failed: " << flags.input();
          return true;
        }
        diagnostics = process->stderr().string_copy();
      }

      const bool keep_unchanged =
          conf->emitter().keep_unchanged_outputs() || RequestsRestat(incoming);

      const ui32 max_links = conf->cache().max_output_links();
      // The hard link and the touch of the linked file change its times: the
      // cache keeps the check of its contents then.
      auto Restore = [&](const Path& cache_file, const String& cache_hash,
                         Immutable content, const String& path) {
        const bool verified = !cache_file.empty() && IsVerified(cache_hash);
        if (!RestoreOutput(cache_file, cache_hash, content, path,
                           keep_unchanged, max_links, incoming, &error)) {
          return false;
        }
        if (verified) {
          Restored(cache_hash);
        }
        return true;
      };
      auto RestoreOutputs = [&](String* failed_path) {
        if (!Restore(entry.object_path, entry.object_hash, entry.object,
                     output_path)) {
          *failed_path = output_path;
          return false;
        }
        if (flags.has_dwo_file() &&
            (!entry.dwo.empty() || !entry.dwo_path.empty())) {
          const String dwo_path = GetDwoPath(incoming);
          if (!Restore(entry.dwo_path, entry.dwo_hash, entry.dwo, dwo_path)) {
            *failed_path = dwo_path;
            return false;
          }
        }
        return true;
      };
      // The linked blob may be compressed or removed by the cleaner since the
      // lookup - then the entry is looked up again, once.
      auto BlobGone = [&] {
        return (!entry.object_path.empty() &&
                !base::File::Exists(entry.object_path)) ||
               (!entry.dwo_path.empty() &&
                !base::File::Exists(entry.dwo_path));
      };
      String failed_path;
      if (!RestoreOutputs(&failed_path)) {
        cache::FileCache::Entry again;
        const String found_hash = entry.handled_hash;
        if (!BlobGone() ||
            !SearchSimpleCache(HandledHash(Immutable(found_hash)), &again,
                               scope)) {
          LOG(ERROR) << "Failed to write file from cache: " << failed_path
                     << " : " << error;
          return false;
        }
        // The diagnostics and the dependencies are of the same entry.
        entry.object = again.object;
        entry.object_path = again.object_path;
        entry.dwo = again.dwo;
        entry.dwo_path = again.dwo_path;
        if (!RestoreOutputs(&failed_path)) {
          LOG(ERROR) << "Failed to write file from cache: " << failed_path
                     << " : " << error;
          return false;
        }
      }

      // The dependency file of the build directory - maybe from another root.
      const Immutable deps = RestoreDeps(entry.deps, flags);
      if (flags.has_deps_file()) {
        const String deps_path = GetDepsPath(incoming);

        if (!base::File::Write(deps_path, deps, &error)) {
          LOG(ERROR) << "Failed to write file from cache: " << deps_path
                     << " : " << error;
          return false;
        }
      }

      if (!time_trace.empty()) {
        const String trace_path = GetTimeTracePath(incoming);
        if (!base::File::Write(trace_path, RestoreTimeTrace(time_trace, flags),
                               &error)) {
          LOG(ERROR) << "Failed to write file from cache: " << trace_path
                     << " : " << error;
          return false;
        }
        STAT(TIME_TRACES_RESTORED);
      }

      // The client doesn't wait for the direct cache, that hashes all
      // headers: it's updated after the reply, like after the compilation.
      std::optional<PendingStores::Store> direct_store;
      if (!source.str.empty()) {
        direct_store.emplace(&direct_stores_, DirectStoreKey(incoming));
      }

      // Before the reply: the client may ask for the stats of the tags at
      // once.
      TagEntry(*incoming, HandledHash(Immutable(entry.handled_hash)));

      net::proto::Status status;
      status.set_code(net::proto::Status::OK);
      // The colors aren't a part of the key.
      status.set_description(flags.color_diagnostics()
                                 ? diagnostics
                                 : StripColors(diagnostics));
      task->connection->ReportStatus(status);
      LOG(VERBOSE) << "Cache hit: " << incoming->flags().input();

      if (direct_store &&
          InputsUnchangedSince(incoming, deps, task->start_time)) {
        UpdateDirectCache(incoming, task->handled_hash, extra_files,
                          deps, task->start_time);
      }
      direct_store.reset();

      MaybeVerifyHit(*incoming, entry, task->start_time);

      restored = true;
      return true;
    };

    // Compiles at once, without the preprocessing: the result is stored only
    // for the direct cache.
    auto CompileWithoutPreprocessing = [&] {
      if (incoming->flags().has_deps_file() &&
          ReadExtraFiles(incoming->flags(), incoming->current_dir(),
                         &task->extra_files)) {
        task->handled_hash = GenerateUniqueHash(incoming->flags());
      }
      failed_tasks_->Push(std::move(*task));
    };

    // The trace of the time is given only by the compilation itself, unless
    // the cache stores the traces - and the time should be the same as
    // without the cache: neither the lookups, nor the preprocessing. The same
    // for the measurements of the whole build.
    if (SkipsLookup(incoming, *conf)) {
      STAT(CACHE_LOOKUP_SKIPPED);
      net::proto::Status status;
      if (!SetupCompiler(incoming->mutable_flags(), &status)) {
        task->connection->ReportStatus(status);
        continue;
      }
      CompileWithoutPreprocessing();
      continue;
    }

    // The result of the previous compilation of the same source - maybe from
    // another checkout - may be being stored right now.
    direct_stores_.WaitFor(DirectStoreKey(incoming));

    bool source_edited = false, source_seen = false;
    if (SearchDirectCache(incoming->flags(), incoming->current_dir(), &entry,
                          scope, &source_edited, &source_seen) &&
        RestoreFromCache(HandledSource(), cache::ExtraFiles{})) {
      if (restored) {
        STAT(DIRECT_CACHE_HIT);
        STAT(TIME_SAVED_BY_DIRECT_HITS, entry.compilation_time);
      }
      continue;
    }

    STAT(DIRECT_CACHE_MISS);

    // Check that we have a compiler of a requested version.
    net::proto::Status status;
    if (!SetupCompiler(incoming->mutable_flags(), &status)) {
      task->connection->ReportStatus(status);
      continue;
    }

    // The preprocessing at the same time would change the times in the traces
    // of the other compilations. The precompiled header has the macros, that
    // the preprocessed source doesn't have: it isn't a key.
    if (incoming->flags().time_trace() || incoming->flags().pch()) {
      CompileWithoutPreprocessing();
      continue;
    }

    // The edited source is unlikely to be found by its preprocessed version -
    // don't waste time on preprocessing, compile locally and store the result
    // only for the direct cache. The same for all sources, if the
    // preprocessing is off - or is done at the same time.
    const auto preprocessing = GetPreprocessing(*conf);
    if ((source_edited ||
         preprocessing != proto::Configuration::Emitter::ALWAYS) &&
        incoming->flags().has_deps_file()) {
      auto& extra_files = task->extra_files;
      if (ReadExtraFiles(incoming->flags(), incoming->current_dir(),
                         &extra_files)) {
        task->handled_hash = GenerateUniqueHash(incoming->flags());

        // Preprocess at the same time only on idle CPUs: otherwise it slows
        // down the other compilations. The new sources - e.g. on the first
        // build - can't be found by the preprocessed versions.
        if (source_edited || !source_seen ||
            preprocessing != proto::Configuration::Emitter::WHEN_IDLE ||
            running_processes_ >= std::thread::hardware_concurrency()) {
          STAT(PREPROCESSING_SKIPPED);
          failed_tasks_->Push(std::move(*task));
          continue;
        }

        // The compilation goes on with the original task, and this thread
        // continues with its copy.
        auto speculation = std::make_shared<Speculation>();
        task->speculation = speculation;
        const auto unique_hash = task->handled_hash;
        Task copy{.connection = task->connection,
                  .message = std::make_unique<base::proto::Local>(*incoming),
                  .extra_files = extra_files,
                  .start_time = task->start_time};
        failed_tasks_->Push(std::move(*task));
        task.emplace(std::move(copy));
        incoming = task->message.get();

        STAT(PARALLEL_PREPROCESSING);
        ++running_processes_;
        auto& source = task->source;
        const bool preprocessed =
            GenerateSource(incoming, task->connection, &source,
                           &task->preprocessed_deps,
                           speculation.get());
        --running_processes_;
        if (!preprocessed) {
          continue;
        }
        // The preprocessing may end after the compilation - and read the
        // inputs, that are saved since: then the preprocessed source doesn't
        // match the object.
        if (!InputsUnchangedSince(incoming,
                                  task->preprocessed_deps,
                                  task->start_time)) {
          STAT(CACHE_STORE_SKIPPED);
          continue;
        }

        const auto handled_hash = GenerateHash(
            incoming->flags(), source, task->extra_files);
        if (SearchSimpleCache(handled_hash, &entry, scope)) {
          if (!speculation->Cancel()) {
            // The compilation has ended already.
            continue;
          }
          task->handled_hash = handled_hash;
          if (RestoreFromCache(source, task->extra_files)) {
            if (restored) {
              STAT(SIMPLE_CACHE_HIT);
              STAT(TIME_SAVED_BY_SIMPLE_HITS, entry.compilation_time);
              STAT(PARALLEL_PREPROCESSING_HIT);
            }
          } else {
            // The compilation is canceled - compile again.
            failed_tasks_->Push(std::move(*task));
          }
          continue;
        }

        if (speculation->SetHandledHash(handled_hash)) {
          // The compilation has ended already - store its result for the
          // simple cache too.
          cache::FileCache::Entry compiled;
          if (SearchSimpleCache(unique_hash, &compiled)) {
            UpdateSimpleCache(handled_hash, compiled);
            TagEntry(*incoming, handled_hash);
          }
        }
        // After the store - the tests wait for it.
        STAT(SIMPLE_CACHE_MISS);
        continue;
      }
    }

    auto& source = task->source;
    if (!GenerateSource(incoming, task->connection, &source,
                        &task->preprocessed_deps)) {
      if (task->connection->IsPeerGone()) {
        STAT(TASK_CANCELED);
        continue;
      }
      failed_tasks_->Push(std::move(*task));
      continue;
    }

    auto& extra_files = task->extra_files;
    if (!ReadExtraFiles(incoming->flags(), incoming->current_dir(),
                        &extra_files)) {
      failed_tasks_->Push(std::move(*task));
      continue;
    }

    auto& handled_hash = task->handled_hash;
    handled_hash = GenerateHash(incoming->flags(), source, extra_files);
    if (SearchSimpleCache(handled_hash, &entry, scope) &&
        RestoreFromCache(source, extra_files)) {
      if (restored) {
        STAT(SIMPLE_CACHE_HIT);
        STAT(TIME_SAVED_BY_SIMPLE_HITS, entry.compilation_time);
      }
      continue;
    }

    STAT(SIMPLE_CACHE_MISS);

    if (!IsRemotable(incoming->flags())) {
      failed_tasks_->Push(std::move(*task));
      continue;
    }

    ui32 shard = Queue::DEFAULT_SHARD;
    if (conf->emitter().has_total_shards()) {
      DCHECK(conf->emitter().total_shards() > 0);
      shard = CalculateShard(handled_hash, conf->emitter().total_shards());
    }
    all_tasks_->Push(std::move(*task), shard);
  }
}

void Emitter::DoLocalExecute(const base::WorkerPool& pool) {
  auto conf = this->conf();

  while (!pool.IsShuttingDown()) {
    Optional&& task = local_tasks_->Pop();
    if (!task) {
      break;
    }

    // The preprocessing at the same time is useless, if the result isn't
    // stored.
    struct Abandon {
      ~Abandon() {
        if (speculation) {
          speculation->Abandon();
        }
      }
      SharedPtr<Speculation> speculation;
    } abandon{task->speculation};

    if (task->connection->IsPeerGone()) {
      STAT(TASK_CANCELED);
      continue;
    }

    base::proto::Local* incoming = task->message.get();

    // Check that we have a compiler of a requested version.
    net::proto::Status status;
    if (!SetupCompiler(incoming->mutable_flags(), &status)) {
      task->connection->ReportStatus(status);
      continue;
    }

    String error;

    ui32 uid =
        incoming->has_user_id() ? incoming->user_id() : base::Process::SAME_UID;

    // Only one task works with the output at a time - until the result is
    // stored into the cache: it's linked from the output.
    OutputLocks::Lock output_lock(&output_locks_, OutputLockKey(incoming));
    if (task->connection->IsPeerGone()) {
      STAT(TASK_CANCELED);
      continue;
    }

    // The result is found by the preprocessed source already.
    const auto& speculation = task->speculation;
    if (speculation && speculation->canceled()) {
      continue;
    }

    // Remove the old outputs: they may be hard-linked with other places, and
    // the compiler shouldn't write into them. Also the split DWARF file may be
    // not generated at all - don't take a stale one into the cache. If the
    // unchanged outputs should be kept, move them aside to compare later.
    const bool keep_unchanged =
        conf->emitter().keep_unchanged_outputs() || RequestsRestat(incoming);
    PreviousOutput previous_output(GetOutputPath(incoming));
    std::optional<PreviousOutput> previous_dwo;
    if (incoming->flags().has_dwo_file()) {
      previous_dwo.emplace(GetDwoPath(incoming));
    }
    // The canceled compilation leaves the previous outputs: the result from the
    // cache may be the same.
    auto RestorePreviousOutputs = [&] {
      previous_output.Restore();
      if (previous_dwo) {
        previous_dwo->Restore();
      }
    };
    if (keep_unchanged) {
      previous_output.Save();
      if (previous_dwo) {
        previous_dwo->Save();
      }
    } else {
      previous_output.Delete();
      if (previous_dwo) {
        previous_dwo->Delete();
      }
    }

    // The stale trace isn't stored, if the compiler doesn't write it.
    if (RestoresTimeTrace(incoming, *conf)) {
      base::File::Delete(GetTimeTracePath(incoming));
    }

    Counter<> counter(Metric::LOCAL_COMPILATION_TIME);
    base::ProcessPtr process =
        CreateProcess(incoming->flags(), uid, Path(incoming->current_dir()));
    // If the client is gone, the compiler is killed before it writes the
    // outputs - they may be produced by another compilation already.
    process->CancelOnDisconnect(task->connection->NativeHandle());
    if (speculation && !speculation->StartCompilation(process.get())) {
      RestorePreviousOutputs();
      continue;
    }
    ++running_processes_;
    const auto compilation_start = Clock::now();
    const bool compiled = process->Run(base::Process::UNLIMITED, &error);
    const auto compilation_time =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - compilation_start)
            .count();
    --running_processes_;
    // The result is restored from the cache instead - by the other thread.
    if (speculation && !speculation->FinishCompilation()) {
      counter.ReportOnDestroy(false);
      RestorePreviousOutputs();
      continue;
    }
    std::optional<cache::FileCache::Entry> entry_to_store;
    Immutable deps;
    if (!compiled) {
      if (process->canceled()) {
        LOG(INFO) << "Compilation is canceled: " << incoming->flags().input();
        STAT(TASK_CANCELED);
        counter.ReportOnDestroy(false);
        RestorePreviousOutputs();
        continue;
      }
      // The compiler hasn't failed - e.g. it can't be started, when
      // "fork()" fails under the load: the client compiles by itself.
      status.set_code(process->exited() ? net::proto::Status::EXECUTION
                                        : net::proto::Status::OVERLOAD);
      if (!process->stderr().empty()) {
        status.set_description(process->stderr());
      } else if (!error.empty()) {
        status.set_description(error);
      } else if (!process->stdout().empty()) {
        status.set_description(process->stdout());
      } else {
        status.set_description("without errors");
      }
    } else {
      status.set_code(net::proto::Status::OK);
      status.set_description(process->stderr());
      LOG(VERBOSE) << "Local compilation successful:  "
                << incoming->flags().input();

      if (previous_output.RestoreIfSame()) {
        STAT(LOCAL_OUTPUT_UNCHANGED);
      }
      if (previous_dwo) {
        previous_dwo->RestoreIfSame();
      }

      const auto& source = task->source;
      const auto& extra_files = task->extra_files;

      counter.Report();
      // The task without the preprocessed source still has the handled hash,
      // if the preprocessing was skipped.
      if (!source.str.empty() || !task->handled_hash.str.empty()) {
        cache::FileCache::Entry entry;
        const auto& flags = incoming->flags();
        // The outputs are linked into the cache, or read, if needed. The
        // dependency file is read before the reply: Ninja removes it.
        entry.object_path = GetOutputPath(incoming);
        if (flags.has_dwo_file() && base::File::Exists(GetDwoPath(incoming))) {
          entry.dwo_path = GetDwoPath(incoming);
        }
        if (base::File::Exists(entry.object_path) &&
            (!flags.has_deps_file() ||
             base::File::Read(GetDepsPath(incoming), &deps))) {
          entry.deps = NormalizeDeps(deps, flags);
          entry.stderr = process->stderr();
          entry.warnings_hash = WarningsHash(flags);
          entry.compilation_time = compilation_time;
          Immutable time_trace;
          if (RestoresTimeTrace(incoming, *conf) &&
              base::File::Read(GetTimeTracePath(incoming), &time_trace)) {
            entry.time_trace = NormalizeTimeTrace(time_trace, flags);
            entry.time_trace_options = TimeTraceOptions(flags);
          }
          auto& handled_hash = task->handled_hash;
          if (handled_hash.str.empty()) {
            handled_hash = GenerateHash(incoming->flags(), source, extra_files);
          }
          entry_to_store = entry;
        }
      }

      STAT(LOCAL_TASK_DONE);
    }

    // The outputs are put into the cache before the reply - the build system
    // may remove them after it.
    bool stored = false;
    if (entry_to_store) {
      // Without the dependency file the headers are known only from the
      // preprocessed source: they may be edited after the preprocessing too.
      List<String> headers;
      const bool headers_known =
          incoming->flags().has_deps_file() ||
          PreprocessedInputs(task->source.str, headers);
      if (headers_known && InputsUnchangedSince(incoming, deps,
                                                task->start_time,
                                                headers)) {
        UpdateSimpleCache(task->handled_hash, *entry_to_store);
        TagEntry(*incoming, task->handled_hash);
        stored = true;
      } else {
        STAT(CACHE_STORE_SKIPPED);
      }
    }

    // The client doesn't wait for the direct cache, that hashes all headers:
    // it's updated after the reply. The next lookups of the same source wait
    // for it.
    std::optional<PendingStores::Store> direct_store;
    if (stored) {
      direct_store.emplace(&direct_stores_, DirectStoreKey(incoming));
    }
    task->connection->ReportStatus(status);

    if (stored) {
      UpdateDirectCache(incoming, task->handled_hash,
                        task->extra_files, deps,
                        task->start_time);
    }

    if (speculation && stored) {
      // The result is stored - only then the preprocessing at the same time
      // may store it for the simple cache.
      cache::string::HandledHash preprocessed_hash;
      speculation->FinishStore(&preprocessed_hash);
      // The preprocessing has missed the cache already.
      if (!preprocessed_hash.str.empty()) {
        UpdateSimpleCache(preprocessed_hash, *entry_to_store);
        TagEntry(*incoming, preprocessed_hash);
      }
    }
    // The next lookups of the same source find the result in both caches.
    direct_store.reset();
  }
}

void Emitter::DoRemoteExecute(const base::WorkerPool& pool, ResolveFn resolver,
                              const ui32 shard) {
  auto conf = this->conf();

  net::EndPointPtr end_point;
  ui32 sleep_period = 1;
  auto Sleep = [&sleep_period]() mutable {
    LOG(INFO) << "Sleeping for " << sleep_period
              << " seconds before next attempt";
    std::this_thread::sleep_for(std::chrono::seconds(sleep_period));
    if (sleep_period < static_cast<ui32>(-1) / 2) {
      sleep_period <<= 1;
    }
  };

  while (!pool.IsShuttingDown()) {
    if (!end_point) {
      {
        Counter<> counter(Metric::REMOTE_RESOLVE_TIME);
        end_point = resolver();
      }
      if (!end_point) {
        Sleep();
        continue;
      }
    }

    Optional&& task =
        all_tasks_->Pop(pool, conf->emitter().shard_queue_limit(), shard);
    if (!task) {
      break;
    }

    if (task->connection->IsPeerGone()) {
      STAT(TASK_CANCELED);
      continue;
    }

    base::proto::Local* incoming = task->message.get();
    auto& source = task->source;
    auto& extra_files = task->extra_files;

    // Check that we have a compiler of a requested version.
    net::proto::Status status;
    if (!SetupCompiler(incoming->mutable_flags(), &status)) {
      task->connection->ReportStatus(status);
      continue;
    }

    // If we're using shards we should have generated source by now.
    DCHECK(!conf->emitter().has_total_shards() || !source.str.empty());

    if (source.str.empty() &&
        !GenerateSource(incoming, task->connection, &source,
                        &task->preprocessed_deps)) {
      if (task->connection->IsPeerGone()) {
        STAT(TASK_CANCELED);
        continue;
      }
      failed_tasks_->Push(std::move(*task));
      continue;
    }

    String error;
    net::ConnectionPtr connection;
    {
      Counter<> counter(Metric::REMOTE_CONNECT_TIME);
      connection = Connect(end_point, &error);
      if (!connection) {
        counter.ReportOnDestroy(false);
        LOG(WARNING) << "Failed to connect to " << end_point->Print() << ": "
                     << error;
        bool& shard_switched = task->changed_shard;

        // |shard_queue_limit| indicates enabled strict sharding that prevents
        // this task from completion in case the remote server has gone. That's
        // why such tasks should be redistributed between other shards.
        //
        // Tasks also shouldn't be redistributed more than one time to prevent
        // tasks hopping between shards in case of all remotes being down.
        if (conf->emitter().has_total_shards() &&
            conf->emitter().shard_queue_limit() && !shard_switched) {
          // Let other shard complete task on connection failure. Do it once.
          shard_switched = true;
          all_tasks_->Push(std::move(*task),
                           FindNewShard(conf->emitter().total_shards(), shard));
        } else {
          // Put into |failed_tasks_| to prevent hanging around in case all
          // remotes are unreachable at once.
          failed_tasks_->Push(std::move(*task));
        }
        Sleep();

        continue;
      }
    }

    sleep_period = 1;

    auto outgoing = std::make_unique<proto::Remote>();
    outgoing->mutable_flags()->CopyFrom(incoming->flags());
    outgoing->set_source(Immutable(source.str).string_copy(false));
    SetExtraFiles(extra_files, outgoing.get());
    auto& handled_hash = task->handled_hash;
    if (handled_hash.str.empty()) {
      handled_hash = GenerateHash(incoming->flags(), source, extra_files);
    }
    outgoing->set_handled_hash(handled_hash.str);

    // Filter outgoing flags.
    auto* flags = outgoing->mutable_flags();
    auto& plugins = *flags->mutable_compiler()->mutable_plugins();
    for (auto& plugin : plugins) {
      plugin.clear_path();
    }
    flags->mutable_compiler()->clear_path();
    flags->clear_output();
    flags->clear_input();
    flags->clear_non_cached();
    flags->clear_non_direct();
    flags->clear_ignored();
    flags->clear_env();
    flags->clear_deps_file();
    flags->clear_deps_target();

    Counter<false> counter(Metric::REMOTE_TIME_WASTED);
    Counter<false> compilation_time_counter(Metric::REMOTE_COMPILATION_TIME);
    if (!connection->SendSync(std::move(outgoing))) {
      all_tasks_->Push(std::move(*task), shard);
      counter.ReportOnDestroy(true);
      continue;
    }

    auto reply = std::make_unique<net::proto::Universal>();
    if (!connection->ReadSync(reply.get())) {
      // Put into |failed_tasks_| in case an oversized protobuf message comes
      // from a remote end.
      failed_tasks_->Push(std::move(*task));
      counter.ReportOnDestroy(true);
      continue;
    }

    if (reply->has_status()) {
      const auto& status = reply->status();
      if (status.code() != net::proto::Status::OK) {
        if (status.code() == net::proto::Status::OVERLOAD) {
          STAT(REMOTE_COMPILATION_REJECTED);
        } else {
          STAT(REMOTE_COMPILATION_FAILED);
        }
        LOG(WARNING) << "Remote compilation failed with error(s):" << std::endl
                     << status.description();
        failed_tasks_->Push(std::move(*task));
        counter.ReportOnDestroy(true);
        continue;
      }
    }

    const String output_path = GetOutputPath(incoming);
    if (reply->has_result()) {
      auto* result = reply->mutable_result();
      if (result->has_from_cache() && result->from_cache()) {
        STAT(REMOTE_CACHE_HIT);
      }
      if (result->has_hash_match() && !result->hash_match()) {
        STAT(HASH_MISMATCH);
      }
      // The client is gone - don't write anything into the build directory:
      // another compilation of the same output may be running already.
      OutputLocks::Lock output_lock(&output_locks_, OutputLockKey(incoming));
      if (task->connection->IsPeerGone()) {
        STAT(TASK_CANCELED);
        continue;
      }

      // The dependency file - from the remote end, or from preprocessing.
      const Immutable deps =
          result->has_deps() ? Immutable(String(result->deps()))
                             : task->preprocessed_deps;
      if (incoming->flags().has_deps_file() &&
          !base::File::Write(GetDepsPath(incoming), deps, &error)) {
        LOG(WARNING) << "Failed to write dependency file "
                     << GetDepsPath(incoming) << ": " << error;
        failed_tasks_->Push(std::move(*task));
        continue;
      }

      if (base::File::Write(output_path,
                            Immutable::WrapString(result->obj()))) {
        if (incoming->has_user_id() &&
            !base::ChangeOwner(output_path, incoming->user_id(), &error)) {
          LOG(ERROR) << "Failed to change owner for " << output_path << ": "
                     << error;
        }

        net::proto::Status status;
        status.set_code(net::proto::Status::OK);
        LOG(INFO) << "Remote compilation successful: "
                  << incoming->flags().input();

        cache::FileCache::Entry entry;
        auto GenerateEntry = [&] {
          String error;

          entry.object = result->release_obj();
          if (incoming->flags().has_deps_file() && deps.empty()) {
            LOG(CACHE_WARNING) << "No dependency file for "
                               << incoming->flags().input();
            return false;
          }
          entry.deps = NormalizeDeps(deps, incoming->flags());
          entry.stderr = Immutable(status.description());
          entry.warnings_hash = WarningsHash(incoming->flags());

          return true;
        };
        compilation_time_counter.Report();

        if (GenerateEntry() &&
            InputsUnchangedSince(incoming, deps, task->start_time)) {
          UpdateSimpleCache(handled_hash, entry);
          UpdateDirectCache(incoming, handled_hash, extra_files, deps,
                            task->start_time);
          TagEntry(*incoming, handled_hash);
        }

        task->connection->ReportStatus(status);
        STAT(REMOTE_TASK_DONE);
        continue;
      }
    } else {
      LOG(WARNING) << "Remote compilation successful, but no results returned: "
                   << output_path;
    }

    // In case this task has crashed the remote end, we will try only local
    // compilation next time.
    failed_tasks_->Push(std::move(*task));
    counter.ReportOnDestroy(true);
  }
}

void Emitter::DoPoll(const base::WorkerPool& pool,
                     Vector<ResolveFn> resolvers) {
  auto conf = this->conf();
  const auto poll_interval = conf->emitter().poll_interval();

  do {
    net::ConnectionPtr connection;

    // Try to get first successful connection to a coordinator.
    for (auto resolver = resolvers.begin(); resolver != resolvers.end();
         ++resolver) {
      net::EndPointPtr end_point = (*resolver)();
      if (!end_point) {
        continue;
      }

      String error;
      connection = Connect(end_point, &error);
      if (!connection) {
        LOG(WARNING) << "Failed to connect to " << end_point->Print() << ": "
                     << error;
        continue;
      } else {
        // In case of success do rotate resolvers to make sure next time we skip
        // 'bad' coordinators and start with a 'good' ones.
        std::rotate(resolvers.begin(), resolver, resolvers.end());
        break;
      }
    }
    if (!connection) {
      continue;
    }

    if (!connection->SendSync(std::make_unique<Configuration>())) {
      std::rotate(resolvers.begin(), resolvers.begin() + 1, resolvers.end());
      continue;
    }

    auto reply = std::make_unique<net::proto::Universal>();
    if (!connection->ReadSync(reply.get())) {
      std::rotate(resolvers.begin(), resolvers.begin() + 1, resolvers.end());
      continue;
    }

    if (reply->has_configuration()) {
      Configuration new_conf(*this->conf());
      const auto& emitter =
          reply->configuration().emitter();
      new_conf.mutable_emitter()->mutable_remotes()->CopyFrom(
          emitter.remotes());
      if (emitter.has_total_shards()) {
        new_conf.mutable_emitter()->set_total_shards(emitter.total_shards());
      }
      if (!Update(new_conf)) {
        // FIXME(ilezhankin): print coordinator's address for clarity.
        LOG(WARNING) << "Failed to update to configuration from coordinator!";
        std::rotate(resolvers.begin(), resolvers.begin() + 1, resolvers.end());
        continue;
      } else {
        // FIXME(ilezhankin): print coordinator's address for clarity.
        LOG(VERBOSE) << "Update to new configuration is successful";
      }
    } else {
      // FIXME(ilezhankin): print coordinator's address for clarity.
      LOG(WARNING) << "Got reply from coordinator, but without configuration!";
      std::rotate(resolvers.begin(), resolvers.begin() + 1, resolvers.end());
      continue;
    }
  } while (!pool.WaitUntilShutdown(std::chrono::seconds(poll_interval)));

  // To ensure all remote tasks are handled before exit - create a pool that is
  // not forced to shut down.
  handle_all_tasks_ = true;
  CHECK(BaseDaemon::Reload());
}

bool Emitter::Check(const Configuration& conf) const {
  if (!CompilationDaemon::Check(conf)) {
    return false;
  }

  if (!conf.has_emitter()) {
    return false;
  }

  const auto& emitter = conf.emitter();

  if (emitter.has_total_shards()) {
    if (emitter.total_shards() > max_total_shards) {
      LOG(ERROR) << "Due to peculiarities of implementation emitter can't use "
                    "more than "
                 << max_total_shards << " total shards";
      return false;
    } else if (emitter.total_shards() < 2) {
      LOG(ERROR) << "Number of total shards must be greater than 1";
      return false;
    } else if (!conf.has_cache() || conf.cache().disabled()) {
      // FIXME: if we don't have cache workers, then we can't
      //        |GenerateSource()| in any separate thread pool.
      //        Also we don't want to pollute sharded remotes with random
      //        builds - so just don't use them for now.
      LOG(ERROR) << "Can't use sharded remotes with disabled local cache";
      return false;
    }
  }

  bool has_active_remote = false;
  for (const auto& remote : emitter.remotes()) {
    if (!remote.disabled()) {
      has_active_remote = true;

      if (remote.has_shard()) {
        if (!emitter.has_total_shards()) {
          LOG(ERROR) << "Remote shouldn't have shard when the number of total "
                        "shards isn't set";
          return false;
        } else if (remote.shard() >= emitter.total_shards()) {
          LOG(ERROR) << "Remote's shard number is out of range (total shards: "
                     << emitter.total_shards() << ")";
          return false;
        }
      } else if (emitter.has_total_shards()) {
        LOG(ERROR) << "All remotes should have their shards when the number of "
                      "total shards is set";
        return false;
      }
    }
  }

  if (emitter.only_failed() && !has_active_remote) {
    // We always should have enabled remotes even with active coordinators.
    // Otherwise we risk to never get any remotes and be silent about it.
    LOG(ERROR) << "Daemon will hang without active remotes and a set flag "
                  "\"emitter.only_failed\"";
    return false;
  }

  return true;
}

bool Emitter::Reload(const proto::Configuration& conf) {
  using Worker = base::WorkerPool::SimpleWorker;

  // Create new pool before swapping, so we won't postpone new tasks.
  auto new_pool = std::make_unique<base::WorkerPool>(!handle_all_tasks_);
  for (const auto& remote : conf.emitter().remotes()) {
    if (remote.disabled()) {
      continue;
    }

    auto resolver = [
      this, host = remote.host(), port = static_cast<ui16>(remote.port()),
      ipv6 = remote.ipv6()
    ]() {
      auto optional = resolver_->Resolve(host, port, ipv6);
      DCHECK(optional);
      optional->Wait();
      return optional->GetValue();
    };

    ui32 shard = remote.has_shard() ? remote.shard() : Queue::DEFAULT_SHARD;
    Worker worker =
        std::bind(&Emitter::DoRemoteExecute, this, _1, resolver, shard);
    new_pool->AddWorker("Remote Execute"_l, worker, remote.threads());
  }
  std::swap(new_pool, remote_workers_);

  auto old_conf = this->conf();

  // In case if new configurations honors strict sharding and has lower number
  // of total shards, make sure tasks from abandoned tasks get redistributed
  // across new shards.
  if (conf.emitter().shard_queue_limit() != Queue::NOT_STRICT_SHARDING &&
      conf.emitter().has_total_shards() &&
      old_conf->emitter().has_total_shards() &&
      conf.emitter().total_shards() < old_conf->emitter().total_shards()) {
    for (ui32 shard = conf.emitter().total_shards();
         shard != old_conf->emitter().total_shards(); ++shard) {
      bool shard_is_empty = false;
      do {
        Optional&& task = all_tasks_->Pop(
            *remote_workers_, std::numeric_limits<ui32>::max(), shard, true);
        if (task) {
          // Once we popped a valid task - put it to appropriate shard according
          // to new number of shards.
          const ui32 new_shard = CalculateShard(task->handled_hash,
                                                conf.emitter().total_shards());
          all_tasks_->Push(std::move(*task), new_shard);
        } else {
          shard_is_empty = true;
        }
      } while (!shard_is_empty);
    }
  }

  return CompilationDaemon::Reload(conf);
}

}  // namespace daemon
}  // namespace dist_clang
