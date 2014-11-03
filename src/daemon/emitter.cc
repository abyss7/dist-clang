#include <daemon/emitter.h>

#include <base/file_utils.h>
#include <base/logging.h>
#include <net/connection.h>
#include <net/end_point.h>

#include <base/using_log.h>

using namespace std::placeholders;

namespace dist_clang {

namespace {

inline String GetOutputPath(const proto::LocalExecute* WEAK_PTR message) {
  DCHECK(message);
  if (message->flags().output()[0] == '/') {
    return message->flags().output();
  } else {
    return message->current_dir() + "/" + message->flags().output();
  }
}

inline String GetDepsPath(const proto::LocalExecute* WEAK_PTR message) {
  DCHECK(message);
  if (message->flags().deps_file()[0] == '/') {
    return message->flags().deps_file();
  } else {
    return message->current_dir() + "/" + message->flags().deps_file();
  }
}

inline bool GenerateSource(const proto::LocalExecute* WEAK_PTR message,
                           file_cache::string::HandledSource* source) {
  proto::Flags pp_flags;

  DCHECK(message);
  pp_flags.CopyFrom(message->flags());
  pp_flags.clear_cc_only();
  pp_flags.set_output("-");
  pp_flags.set_action("-E");

  base::ProcessPtr process =
      daemon::BaseDaemon::CreateProcess(pp_flags, message->current_dir());
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
    : BaseDaemon(configuration) {
  using Worker = base::WorkerPool::SimpleWorker;

  CHECK(conf_.has_emitter());

  workers_.reset(new base::WorkerPool);
  all_tasks_.reset(new Queue);
  cache_tasks_.reset(new Queue);
  failed_tasks_.reset(new Queue);

  local_tasks_.reset(new QueueAggregator);
  local_tasks_->Aggregate(failed_tasks_.get());
  if (!conf_.emitter().only_failed()) {
    local_tasks_->Aggregate(all_tasks_.get());
  }

  {
    Worker worker = std::bind(&Emitter::DoLocalExecute, this, _1);
    workers_->AddWorker(worker, conf_.emitter().threads());
  }

  if (conf_.has_cache() && !conf_.cache().disabled()) {
    Worker worker = std::bind(&Emitter::DoCheckCache, this, _1);
    workers_->AddWorker(worker, std::thread::hardware_concurrency() * 2);
  }

  for (const auto& remote : conf_.emitter().remotes()) {
    if (!remote.disabled()) {
      auto end_point = resolver_->Resolve(remote.host(), remote.port());
      Worker worker = std::bind(&Emitter::DoRemoteExecute, this, _1, end_point);
      workers_->AddWorker(worker, remote.threads());
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
  if (!Listen(conf_.emitter().socket_path(), &error)) {
    LOG(ERROR) << "Failed to listen on " << conf_.emitter().socket_path()
               << " : " << error;
    return false;
  }

  if (conf_.emitter().only_failed()) {
    bool has_active_remote = false;

    for (const auto& remote : conf_.emitter().remotes()) {
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

  return BaseDaemon::Initialize();
}

bool Emitter::HandleNewMessage(net::ConnectionPtr connection, Universal message,
                               const proto::Status& status) {
  using namespace file_cache::string;

  if (!message->IsInitialized()) {
    LOG(INFO) << message->InitializationErrorString();
    return false;
  }

  if (status.code() != proto::Status::OK) {
    LOG(ERROR) << status.description();
    return connection->ReportStatus(status);
  }

  if (message->HasExtension(proto::LocalExecute::extension)) {
    Message execute(message->ReleaseExtension(proto::LocalExecute::extension));
    if (conf_.has_cache() && !conf_.cache().disabled()) {
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

void Emitter::DoCheckCache(const std::atomic<bool>& is_shutting_down) {
  using namespace file_cache::string;

  while (!is_shutting_down) {
    Optional&& task = cache_tasks_->Pop();
    if (!task) {
      break;
    }
    proto::LocalExecute* incoming = std::get<MESSAGE>(*task).get();
    FileCache::Entry entry;

    auto RestoreFromCache = [&](const HandledSource& source) {
      String error;
      const String output_path = GetOutputPath(incoming);

      if (!base::WriteFile(output_path, entry.object)) {
        LOG(ERROR) << "Failed to restore file from cache: " << output_path;
        return false;
      }
      if (incoming->has_user_id() &&
          !base::ChangeOwner(output_path, incoming->user_id(), &error)) {
        LOG(ERROR) << "Failed to change owner for " << output_path << ": "
                   << error;
      }

      // TODO: restore deps file.

      if (!source.str.empty()) {
        UpdateDirectCache(incoming, source, entry);
      }

      proto::Status status;
      status.set_code(proto::Status::OK);
      status.set_description(entry.stderr);
      std::get<CONNECTION>(*task)->ReportStatus(status);

      return true;
    };

    if (SearchDirectCache(incoming->flags(), incoming->current_dir(), &entry) &&
        RestoreFromCache(HandledSource())) {
      continue;
    }

    auto& source = std::get<SOURCE>(*task);
    if (!GenerateSource(incoming, &source)) {
      failed_tasks_->Push(std::move(*task));
      continue;
    }

    if (SearchSimpleCache(incoming->flags(), source, &entry) &&
        RestoreFromCache(source)) {
      continue;
    }

    all_tasks_->Push(std::move(*task));
  }
}

void Emitter::DoLocalExecute(const std::atomic<bool>& is_shutting_down) {
  while (!is_shutting_down) {
    Optional&& task = local_tasks_->Pop();
    if (!task) {
      break;
    }
    proto::LocalExecute* incoming = std::get<MESSAGE>(*task).get();

    // Check that we have a compiler of a requested version.
    proto::Status status;
    if (!SetupCompiler(incoming->mutable_flags(), &status)) {
      std::get<CONNECTION>(*task)->ReportStatus(status);
      continue;
    }

    String error;

    ui32 uid =
        incoming->has_user_id() ? incoming->user_id() : base::Process::SAME_UID;
    base::ProcessPtr process =
        CreateProcess(incoming->flags(), uid, incoming->current_dir());
    if (!process->Run(base::Process::UNLIMITED, &error)) {
      status.set_code(proto::Status::EXECUTION);
      if (!process->stderr().empty()) {
        status.set_description(process->stderr());
      } else if (!error.empty()) {
        status.set_description(error);
      } else {
        status.set_description("without errors");
      }
    } else {
      status.set_code(proto::Status::OK);
      status.set_description(process->stderr());
      LOG(INFO) << "Local compilation successful:  "
                << incoming->flags().input();

      const auto& source = std::get<SOURCE>(*task);

      if (!source.str.empty()) {
        FileCache::Entry entry;
        if (base::ReadFile(GetOutputPath(incoming), &entry.object) &&
            (!incoming->flags().has_deps_file() ||
             base::ReadFile(GetDepsPath(incoming), &entry.deps))) {
          entry.stderr = process->stderr();
          UpdateSimpleCache(incoming->flags(), source, entry);
          UpdateDirectCache(incoming, source, entry);
        }
      }
    }

    std::get<CONNECTION>(*task)->ReportStatus(status);
  }
}

void Emitter::DoRemoteExecute(const std::atomic<bool>& is_shutting_down,
                              net::EndPointResolver::Optional end_point) {
  if (!end_point || !(*end_point)) {
    // TODO: do re-resolve |end_point| periodically, since the network
    // configuration may change on runtime.
    return;
  }

  end_point->Wait();

  while (!is_shutting_down) {
    Optional&& task = all_tasks_->Pop();
    if (!task) {
      break;
    }
    proto::LocalExecute* incoming = std::get<MESSAGE>(*task).get();
    auto& source = std::get<SOURCE>(*task);

    UniquePtr<proto::RemoteExecute> outgoing(new proto::RemoteExecute);
    if (source.str.empty() && !GenerateSource(incoming, &source)) {
      failed_tasks_->Push(std::move(*task));
      continue;
    }

    auto connection = Connect(end_point->GetValue());
    if (!connection) {
      all_tasks_->Push(std::move(*task));
      continue;
    }

    outgoing->mutable_flags()->CopyFrom(incoming->flags());
    outgoing->set_source(source);

    // Filter outgoing flags.
    outgoing->mutable_flags()->mutable_compiler()->clear_path();
    outgoing->mutable_flags()->clear_output();
    outgoing->mutable_flags()->clear_input();
    outgoing->mutable_flags()->clear_non_cached();
    outgoing->mutable_flags()->clear_deps_file();

    if (!connection->SendSync(std::move(outgoing))) {
      all_tasks_->Push(std::move(*task));
      continue;
    }

    Universal reply(new proto::Universal);
    if (!connection->ReadSync(reply.get())) {
      all_tasks_->Push(std::move(*task));
      continue;
    }

    if (reply->HasExtension(proto::Status::extension)) {
      const auto& status = reply->GetExtension(proto::Status::extension);
      if (status.code() != proto::Status::OK) {
        LOG(WARNING) << "Remote compilation failed with error(s):" << std::endl
                     << status.description();
        failed_tasks_->Push(std::move(*task));
        continue;
      }
    }

    const String output_path = GetOutputPath(incoming);
    if (reply->HasExtension(proto::RemoteResult::extension)) {
      const auto& result = reply->GetExtension(proto::RemoteResult::extension);
      if (base::WriteFile(output_path, result.obj())) {
        proto::Status status;
        status.set_code(proto::Status::OK);
        LOG(INFO) << "Remote compilation successful: "
                  << incoming->flags().input();

        FileCache::Entry entry;
        auto GenerateEntry = [&] {
          entry.object = result.obj();
          if (result.has_deps()) {
            entry.deps = result.deps();
          } else if (incoming->flags().has_deps_file() &&
                     !base::ReadFile(GetDepsPath(incoming), &entry.deps)) {
            return false;
          }
          entry.stderr = status.description();

          return true;
        };

        if (GenerateEntry()) {
          UpdateSimpleCache(incoming->flags(), source, entry);
          UpdateDirectCache(incoming, source, entry);
        }

        std::get<CONNECTION>(*task)->ReportStatus(status);
        continue;
      }
    } else {
      LOG(WARNING) << "Remote compilation successful, but no results returned: "
                   << output_path;
    }

    // In case this task has crashed the remote end, we will try only local
    // compilation next time.
    failed_tasks_->Push(std::move(*task));
  }
}

}  // namespace daemon
}  // namespace dist_clang
