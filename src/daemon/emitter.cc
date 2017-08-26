#include <daemon/emitter.h>

#include <base/file/file.h>
#include <base/logging.h>
#include <base/process.h>
#include <net/connection.h>
#include <net/end_point.h>
#include <perf/counter.h>
#include <perf/stat_reporter.h>
#include <perf/stat_service.h>

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

inline String GetDepsPath(const base::proto::Local* WEAK_PTR message) {
  DCHECK(message);
  if (message->flags().deps_file()[0] == '/') {
    return message->flags().deps_file();
  } else {
    return message->current_dir() + "/" + message->flags().deps_file();
  }
}

inline bool GenerateSource(const base::proto::Local* WEAK_PTR message,
                           cache::string::HandledSource* source) {
  Counter<> preprocess_time_counter(Metric::PREPROCESS_TIME);
  base::proto::Flags pp_flags;

  DCHECK(message);
  pp_flags.CopyFrom(message->flags());
  pp_flags.clear_cc_only();
  pp_flags.set_output("-");
  pp_flags.set_action("-E");

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

  if (!process->Run(10)) {
    return false;
  }

  if (source) {
    source->str.assign(process->stdout());
  }

  return true;
}

}  // namespace

namespace daemon {

const ui32 Emitter::max_total_shards = 1024u;

Emitter::Emitter(const proto::Configuration& conf) : CompilationDaemon(conf) {
  using Worker = base::WorkerPool::SimpleWorker;

  CHECK(conf.has_emitter());

  workers_ = std::make_unique<base::WorkerPool>();
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
    workers_->AddWorker("Local Execute Worker"_l, worker,
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
      coordinator_workers_->AddWorker("Coordinator Poll Worker"_l, worker);
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
}

bool Emitter::Initialize() {
  auto conf = this->conf();

  String error;
  if (!Listen(conf->emitter().socket_path(), &error)) {
    LOG(ERROR) << "Emitter failed to listen on "
               << conf->emitter().socket_path() << " : " << error;
    return false;
  }

  return CompilationDaemon::Initialize();
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

  if (message->HasExtension(base::proto::Local::extension)) {
    Message execute(message->ReleaseExtension(base::proto::Local::extension));
    if (conf->has_cache() && !conf->cache().disabled()) {
      return cache_tasks_->Push(
          std::make_tuple(connection, std::move(execute), HandledSource(),
                          cache::ExtraFiles{}, HandledHash(), false));
    } else {
      return all_tasks_->Push(
          std::make_tuple(connection, std::move(execute), HandledSource(),
                          cache::ExtraFiles{}, HandledHash(), false));
    }
  }

  NOTREACHED();
  return false;
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

    if (std::get<CONNECTION>(*task)->IsClosed()) {
      continue;
    }

    base::proto::Local* incoming = std::get<MESSAGE>(*task).get();
    cache::FileCache::Entry entry;

    auto RestoreFromCache = [&](const HandledSource& source,
                                const cache::ExtraFiles& extra_files) {
      String error;
      const String output_path = GetOutputPath(incoming);

      if (!base::File::Write(output_path, entry.object, &error)) {
        LOG(ERROR) << "Failed to write file from cache: " << output_path
                   << " : " << error;
        return false;
      }
      if (incoming->has_user_id() &&
          !base::ChangeOwner(output_path, incoming->user_id(), &error)) {
        LOG(ERROR) << "Failed to change owner for " << output_path << " : "
                   << error;
      }

      if (incoming->flags().has_deps_file()) {
        DCHECK(!entry.deps.empty());

        const String deps_path = GetDepsPath(incoming);

        if (!base::File::Write(deps_path, entry.deps, &error)) {
          LOG(ERROR) << "Failed to write file from cache: " << deps_path
                     << " : " << error;
          return false;
        }
      }

      if (!source.str.empty()) {
        UpdateDirectCache(incoming, source, extra_files, entry);
      }

      net::proto::Status status;
      status.set_code(net::proto::Status::OK);
      status.set_description(entry.stderr);
      std::get<CONNECTION>(*task)->ReportStatus(status);
      LOG(INFO) << "Cache hit: " << incoming->flags().input();

      return true;
    };

    if (SearchDirectCache(incoming->flags(), incoming->current_dir(), &entry) &&
        RestoreFromCache(HandledSource(), cache::ExtraFiles{})) {
      STAT(DIRECT_CACHE_HIT);
      continue;
    }

    STAT(DIRECT_CACHE_MISS);

    // Check that we have a compiler of a requested version.
    net::proto::Status status;
    if (!SetupCompiler(incoming->mutable_flags(), &status)) {
      std::get<CONNECTION>(*task)->ReportStatus(status);
      continue;
    }

    auto& source = std::get<SOURCE>(*task);
    if (!GenerateSource(incoming, &source)) {
      failed_tasks_->Push(std::move(*task));
      continue;
    }

    auto& extra_files = std::get<EXTRA_FILES>(*task);
    if (!ReadExtraFiles(incoming->flags(), incoming->current_dir(),
                        &extra_files)) {
      failed_tasks_->Push(std::move(*task));
      continue;
    }

    auto& handled_hash = std::get<HANDLED_HASH>(*task);
    handled_hash = GenerateHash(incoming->flags(), source, extra_files);
    if (SearchSimpleCache(handled_hash, &entry) &&
        RestoreFromCache(source, extra_files)) {
      STAT(SIMPLE_CACHE_HIT);
      continue;
    }

    STAT(SIMPLE_CACHE_MISS);

    ui32 shard = Queue::DEFAULT_SHARD;
    if (conf->emitter().has_total_shards()) {
      DCHECK(conf->emitter().total_shards() > 0);
      shard = CalculateShard(handled_hash, conf->emitter().total_shards());
    }
    all_tasks_->Push(std::move(*task), shard);
  }
}

void Emitter::DoLocalExecute(const base::WorkerPool& pool) {
  while (!pool.IsShuttingDown()) {
    Optional&& task = local_tasks_->Pop();
    if (!task) {
      break;
    }

    if (std::get<CONNECTION>(*task)->IsClosed()) {
      continue;
    }

    base::proto::Local* incoming = std::get<MESSAGE>(*task).get();

    // Check that we have a compiler of a requested version.
    net::proto::Status status;
    if (!SetupCompiler(incoming->mutable_flags(), &status)) {
      std::get<CONNECTION>(*task)->ReportStatus(status);
      continue;
    }

    String error;

    ui32 uid =
        incoming->has_user_id() ? incoming->user_id() : base::Process::SAME_UID;
    Counter<> counter(Metric::LOCAL_COMPILATION_TIME);
    base::ProcessPtr process =
        CreateProcess(incoming->flags(), uid, Path(incoming->current_dir()));
    if (!process->Run(base::Process::UNLIMITED, &error)) {
      status.set_code(net::proto::Status::EXECUTION);
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
      LOG(INFO) << "Local compilation successful:  "
                << incoming->flags().input();

      const auto& source = std::get<SOURCE>(*task);
      const auto& extra_files = std::get<EXTRA_FILES>(*task);

      counter.Report();
      if (!source.str.empty()) {
        cache::FileCache::Entry entry;
        if (base::File::Read(GetOutputPath(incoming), &entry.object) &&
            (!incoming->flags().has_deps_file() ||
             base::File::Read(GetDepsPath(incoming), &entry.deps))) {
          entry.stderr = process->stderr();
          auto& handled_hash = std::get<HANDLED_HASH>(*task);
          if (handled_hash.str.empty()) {
            handled_hash = GenerateHash(incoming->flags(), source, extra_files);
          }
          UpdateSimpleCache(handled_hash, entry);
          UpdateDirectCache(incoming, source, extra_files, entry);
        }
      }

      STAT(LOCAL_TASK_DONE);
    }

    std::get<CONNECTION>(*task)->ReportStatus(status);
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

    if (std::get<CONNECTION>(*task)->IsClosed()) {
      continue;
    }

    base::proto::Local* incoming = std::get<MESSAGE>(*task).get();
    auto& source = std::get<SOURCE>(*task);
    auto& extra_files = std::get<EXTRA_FILES>(*task);

    // Check that we have a compiler of a requested version.
    net::proto::Status status;
    if (!SetupCompiler(incoming->mutable_flags(), &status)) {
      std::get<CONNECTION>(*task)->ReportStatus(status);
      continue;
    }

    // If we're using shards we should have generated source by now.
    DCHECK(!conf->emitter().has_total_shards() || !source.str.empty());

    if (source.str.empty() && !GenerateSource(incoming, &source)) {
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
        bool& shard_switched = std::get<CHANGED_SHARD>(*task);

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
    auto& handled_hash = std::get<HANDLED_HASH>(*task);
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
    flags->clear_deps_file();

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

    if (reply->HasExtension(net::proto::Status::extension)) {
      const auto& status = reply->GetExtension(net::proto::Status::extension);
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
    if (reply->HasExtension(proto::Result::extension)) {
      auto* result = reply->MutableExtension(proto::Result::extension);
      if (result->has_from_cache() && result->from_cache()) {
        STAT(REMOTE_CACHE_HIT);
      }
      if (result->has_hash_match() && !result->hash_match()) {
        STAT(HASH_MISMATCH);
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
          if (result->has_deps()) {
            entry.deps = result->release_deps();
          } else if (incoming->flags().has_deps_file() &&
                     !base::File::Read(GetDepsPath(incoming), &entry.deps,
                                       &error)) {
            LOG(CACHE_WARNING) << "Can't read deps file "
                               << GetDepsPath(incoming) << " : " << error;
            return false;
          }
          entry.stderr = Immutable(status.description());

          return true;
        };
        compilation_time_counter.Report();

        if (GenerateEntry()) {
          UpdateSimpleCache(handled_hash, entry);
          UpdateDirectCache(incoming, source, extra_files, entry);
        }

        std::get<CONNECTION>(*task)->ReportStatus(status);
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

    if (reply->HasExtension(Configuration::extension)) {
      Configuration new_conf(*this->conf());
      const auto& emitter =
          reply->GetExtension(Configuration::extension).emitter();
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
    new_pool->AddWorker("Remote Execute Worker"_l, worker, remote.threads());
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
          const ui32 new_shard = CalculateShard(std::get<HANDLED_HASH>(*task),
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
