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

using namespace std::placeholders;

namespace dist_clang {

namespace {

static std::vector<std::string> GetAdditionalSourceFiles(
    base::proto::Local* message) {
  std::vector<std::string> files;
  if (message->flags().has_sanitize_blacklist()) {
    auto blacklist = message->current_dir();
    blacklist += '/';
    blacklist += message->flags().sanitize_blacklist();

    files.push_back(blacklist);
  }
  return files;
}

static std::unique_ptr<std::vector<cache::string::UnhandledSource>>
ReadUnhandledSources(base::proto::Local* message) {
  auto files = GetAdditionalSourceFiles(message);
  std::unique_ptr<std::vector<cache::string::UnhandledSource>> result(
      new std::vector<cache::string::UnhandledSource>);
  for (auto& it : files) {
    cache::string::UnhandledSource content;
    if (!base::File::Read(it, &content.str)) {
      return std::unique_ptr<std::vector<cache::string::UnhandledSource>>();
    }
    result->push_back(content);
  }
  return result;
}

static std::vector<cache::string::HandledSource> HandleSources(
    const std::vector<cache::string::UnhandledSource>& src) {
  std::vector<cache::string::HandledSource> result;

  std::transform(std::cbegin(src), std::cend(src), std::back_inserter(result),
                 [](const cache::string::UnhandledSource& src)
                     -> cache::string::HandledSource {
    return cache::string::HandledSource(src.str);
  });

  return result;
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
  base::proto::Flags pp_flags;

  DCHECK(message);
  pp_flags.CopyFrom(message->flags());
  pp_flags.clear_cc_only();
  pp_flags.set_output("-");
  pp_flags.set_action("-E");

  // Clang plugins can't affect source code.
  pp_flags.mutable_compiler()->clear_plugins();

  base::ProcessPtr process;
  if (message->has_user_id()) {
    process = daemon::CompilationDaemon::CreateProcess(
        pp_flags, message->user_id(), Immutable(message->current_dir()));
  } else {
    process = daemon::CompilationDaemon::CreateProcess(
        pp_flags, Immutable(message->current_dir()));
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

Emitter::Emitter(const proto::Configuration& configuration)
    : CompilationDaemon(configuration) {
  using Worker = base::WorkerPool::SimpleWorker;
  auto config = conf();
  CHECK(config->has_emitter());

  workers_.reset(new base::WorkerPool);
  all_tasks_.reset(new Queue);
  cache_tasks_.reset(new Queue);
  failed_tasks_.reset(new Queue);

  local_tasks_.reset(new QueueAggregator);
  local_tasks_->Aggregate(failed_tasks_.get());
  if (!config->emitter().only_failed()) {
    local_tasks_->Aggregate(all_tasks_.get());
  }

  {
    Worker worker = std::bind(&Emitter::DoLocalExecute, this, _1);
    workers_->AddWorker("Local Execute Worker"_l, worker,
                        config->emitter().threads());
  }

  if (config->has_cache() && !config->cache().disabled()) {
    Worker worker = std::bind(&Emitter::DoCheckCache, this, _1);
    if (config->cache().has_threads()) {
      workers_->AddWorker("Cache Worker"_l, worker, config->cache().threads());
    } else {
      workers_->AddWorker("Cache Worker"_l, worker,
                          std::thread::hardware_concurrency());
    }
  }

  for (const auto& remote : config->emitter().remotes()) {
    if (!remote.disabled()) {
      auto resolver = [
        this,
        host = remote.host(),
        port = static_cast<ui16>(remote.port()),
        ipv6 = remote.ipv6()
      ]() {
        auto optional = resolver_->Resolve(host, port, ipv6);
        DCHECK(optional);
        optional->Wait();
        return optional->GetValue();
      };
      Worker worker = std::bind(&Emitter::DoRemoteExecute, this, _1, resolver);
      workers_->AddWorker("Remote Execute Worker"_l, worker, remote.threads());
    }
  }
}

Emitter::~Emitter() {
  all_tasks_->Close();
  cache_tasks_->Close();
  failed_tasks_->Close();
  local_tasks_->Close();
  workers_.reset();
}

bool Emitter::Initialize() {
  String error;
  auto config = conf();
  if (!Listen(config->emitter().socket_path(), &error)) {
    LOG(ERROR) << "Failed to listen on " << config->emitter().socket_path()
               << " : " << error;
    return false;
  }

  if (config->emitter().only_failed()) {
    bool has_active_remote = false;

    for (const auto& remote : config->emitter().remotes()) {
      if (!remote.disabled()) {
        has_active_remote = true;
        break;
      }
    }

    if (!has_active_remote) {
      LOG(ERROR) << "Daemon will hang without active remotes and set flag "
                    "\"emitter.only_failed\"";
      return false;
    }
  }

  return CompilationDaemon::Initialize();
}

bool Emitter::HandleNewMessage(net::ConnectionPtr connection, Universal message,
                               const net::proto::Status& status) {
  using namespace cache::string;
  auto config = conf();
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
    if (config->has_cache() && !config->cache().disabled()) {
      return cache_tasks_->Push(
          std::make_tuple(connection, std::move(execute), HandledSource()));
    } else {
      return all_tasks_->Push(
          std::make_tuple(connection, std::move(execute), HandledSource()));
    }
  }

  NOTREACHED();
  return false;
}

void Emitter::DoCheckCache(const Atomic<bool>& is_shutting_down) {
  using namespace cache::string;

  while (!is_shutting_down) {
    Optional&& task = cache_tasks_->Pop();
    if (!task) {
      break;
    }

    if (std::get<CONNECTION>(*task)->IsClosed()) {
      continue;
    }

    base::proto::Local* incoming = std::get<MESSAGE>(*task).get();
    cache::FileCache::Entry entry;

    auto RestoreFromCache =
        [&](const HandledSource& source,
            std::vector<HandledSource>& additional_sources) {
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
        UpdateDirectCache(incoming, source, entry, additional_sources);
      }

      net::proto::Status status;
      status.set_code(net::proto::Status::OK);
      status.set_description(entry.stderr);
      std::get<CONNECTION>(*task)->ReportStatus(status);
      LOG(INFO) << "Cache hit: " << incoming->flags().input();

      return true;
    };

    auto additional = ReadUnhandledSources(incoming);

    if (!additional) {
      LOG(ERROR) << "Can't open additional sources";
      failed_tasks_->Push(std::move(*task));
      continue;
    }

    auto handled_additional = HandleSources(*additional.get());

    if (SearchDirectCache(incoming->flags(), incoming->current_dir(), &entry,
                          *additional.get()) &&
        RestoreFromCache(HandledSource(), handled_additional)) {
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

    if (SearchSimpleCache(incoming->flags(), source, &entry,
                          handled_additional) &&
        RestoreFromCache(source, handled_additional)) {
      STAT(SIMPLE_CACHE_HIT);
      continue;
    }

    STAT(SIMPLE_CACHE_MISS);

    all_tasks_->Push(std::move(*task));
  }
}

void Emitter::DoLocalExecute(const Atomic<bool>& is_shutting_down) {
  while (!is_shutting_down) {
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
    base::ProcessPtr process = CreateProcess(
        incoming->flags(), uid, Immutable(incoming->current_dir()));
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

      if (!source.str.empty()) {
        cache::FileCache::Entry entry;
        auto unhandled = ReadUnhandledSources(incoming);
        if (unhandled) {
          auto handled = HandleSources(*unhandled.get());
          if (base::File::Read(GetOutputPath(incoming), &entry.object) &&
              (!incoming->flags().has_deps_file() ||
               base::File::Read(GetDepsPath(incoming), &entry.deps))) {
            entry.stderr = process->stderr();
            UpdateSimpleCache(incoming->flags(), source, entry, handled);
            UpdateDirectCache(incoming, source, entry, handled);
          }
        }
      }

      STAT(LOCAL_TASK_DONE);
    }

    std::get<CONNECTION>(*task)->ReportStatus(status);
  }
}

void Emitter::DoRemoteExecute(const Atomic<bool>& is_shutting_down,
                              ResolveFn resolver) {
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

  while (!is_shutting_down) {
    if (!end_point) {
      end_point = resolver();
      if (!end_point) {
        Sleep();
        continue;
      }
    }

    Optional&& task = all_tasks_->Pop();
    if (!task) {
      break;
    }

    if (std::get<CONNECTION>(*task)->IsClosed()) {
      continue;
    }

    base::proto::Local* incoming = std::get<MESSAGE>(*task).get();
    auto& source = std::get<SOURCE>(*task);

    // Check that we have a compiler of a requested version.
    net::proto::Status status;
    if (!SetupCompiler(incoming->mutable_flags(), &status)) {
      std::get<CONNECTION>(*task)->ReportStatus(status);
      continue;
    }

    UniquePtr<proto::Remote> outgoing(new proto::Remote);
    if (source.str.empty() && !GenerateSource(incoming, &source)) {
      failed_tasks_->Push(std::move(*task));
      continue;
    }

    if (incoming->has_flags() && incoming->flags().has_sanitize_blacklist()) {
      Immutable content;
      auto blacklist = incoming->flags().sanitize_blacklist();
      std::string sanitize_blacklist_file_path = incoming->current_dir();
      sanitize_blacklist_file_path += "/";
      sanitize_blacklist_file_path += blacklist;
      base::File::Read(sanitize_blacklist_file_path, &content);
      outgoing->set_sanitize_blacklist_content(content.string_copy());
    }

    String error;
    auto connection = Connect(end_point, &error);
    if (!connection) {
      LOG(WARNING) << "Failed to connect to " << end_point->Print() << ": "
                   << error;
      // Put into |failed_tasks_| to prevent hanging around in case all
      // remotes are unreachable at once.
      failed_tasks_->Push(std::move(*task));
      Sleep();

      continue;
    }

    sleep_period = 1;

    outgoing->mutable_flags()->CopyFrom(incoming->flags());
    outgoing->set_source(Immutable(source.str).string_copy(false));

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

    perf::Counter<perf::StatReporter, false> counter(
        perf::proto::Metric::REMOTE_TIME_WASTED);
    if (!connection->SendSync(std::move(outgoing))) {
      all_tasks_->Push(std::move(*task));
      counter.ReportOnDestroy(true);
      continue;
    }

    Universal reply(new net::proto::Universal);
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

        if (GenerateEntry()) {
          auto unhandled = ReadUnhandledSources(incoming);
          if (unhandled) {
            auto handled = HandleSources(*unhandled.get());
            UpdateSimpleCache(incoming->flags(), source, entry, handled);
            UpdateDirectCache(incoming, source, entry, handled);
          }
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

}  // namespace daemon
}  // namespace dist_clang
