#include <daemon/absorber.h>

#include <base/file/file.h>
#include <base/file_utils.h>
#include <base/logging.h>
#include <base/process.h>
#include <base/protobuf_utils.h>
#include <base/temporary_dir.h>
#include <net/connection.h>

#include <base/using_log.h>

using namespace std::placeholders;

namespace dist_clang {
namespace daemon {

Absorber::Absorber(const Configuration& conf) : CompilationDaemon(conf) {
  using Worker = base::WorkerPool::SimpleWorker;
  CHECK(conf.has_absorber() && !conf.absorber().local().disabled());

  workers_ = std::make_unique<base::WorkerPool>();
  tasks_ = std::make_unique<Queue>(conf.pool_capacity());

  {
    Worker worker = std::bind(&Absorber::DoExecute, this, _1);
    workers_->AddWorker("Execute Worker"_l, worker,
                        conf.absorber().local().threads());
  }
}

Absorber::~Absorber() {
  tasks_->Close();
  workers_.reset();
}

bool Absorber::Initialize() {
  auto conf = this->conf();

  String error;
  const auto& local = conf->absorber().local();
  if (!Listen(local.host(), local.port(), local.ipv6(), &error)) {
    LOG(ERROR) << "Absorber failed to listen on " << local.host() << ":"
               << local.port() << " : " << error;
    return false;
  }

  if (conf->has_cache() && conf->cache().has_direct() &&
      conf->cache().direct()) {
    LOG(WARNING) << "Absorber doesn't use the Direct Cache mode. The flag "
                    "\"cache.direct\" will be ignored";
  }

  return CompilationDaemon::Initialize();
}

bool Absorber::HandleNewMessage(net::ConnectionPtr connection,
                                Universal message,
                                const net::proto::Status& status) {
  if (!message->IsInitialized()) {
    LOG(INFO) << message->InitializationErrorString();
    return false;
  }

  if (status.code() != net::proto::Status::OK) {
    LOG(ERROR) << status.description();
    return connection->ReportStatus(status);
  }

  if (message->HasExtension(proto::Remote::extension)) {
    Message execute(message->ReleaseExtension(proto::Remote::extension));
    DCHECK(!execute->flags().compiler().has_path());
    if (execute->has_source()) {
      if (tasks_->Push(Task{connection, std::move(execute)})) {
        return true;
      } else {
        net::proto::Status overload;
        overload.set_code(net::proto::Status::OVERLOAD);
        overload.set_description("Tasks queue reached limit");
        connection->ReportStatus(overload);
        return false;
      }
    }
  }

  NOTREACHED();
  return false;
}

cache::ExtraFiles Absorber::GetExtraFiles(const proto::Remote* message) {
  DCHECK(message);

  cache::ExtraFiles extra_files;

  if (message->has_sanitize_blacklist()) {
    extra_files.emplace(cache::SANITIZE_BLACKLIST,
                        Immutable::WrapString(message->sanitize_blacklist()));
  }

  return extra_files;
}

bool Absorber::PrepareExtraFilesForCompiler(
    const cache::ExtraFiles& extra_files, const String& temp_dir_path,
    base::proto::Flags* flags, net::proto::Status* status) {
  DCHECK(flags);
  DCHECK(status);

  auto sanitize_blacklist = extra_files.find(cache::SANITIZE_BLACKLIST);
  if (sanitize_blacklist != extra_files.end()) {
    String sanitize_blacklist_file = temp_dir_path + "/sanitize_blacklist";
    if (!base::File::Write(sanitize_blacklist_file,
                           sanitize_blacklist->second)) {
      LOG(WARNING) << "Failed to write sanitize blacklist file "
                   << sanitize_blacklist_file;
      status->set_code(net::proto::Status::EXECUTION);
      status->set_description("Failed to write sanitize blacklist file " +
                              sanitize_blacklist_file);
      return false;
    }
    flags->set_sanitize_blacklist(sanitize_blacklist_file);
  }

  return true;
}

void Absorber::DoExecute(const base::WorkerPool& pool) {
  using namespace cache::string;

  while (!pool.IsShuttingDown()) {
    Optional&& task = tasks_->Pop();
    if (!task) {
      break;
    }

    if (task->first->IsClosed()) {
      continue;
    }

    proto::Remote* incoming = task->second.get();
    auto source = Immutable::WrapString(incoming->source());
    auto extra_files = GetExtraFiles(incoming);

    incoming->mutable_flags()->set_output("-");
    incoming->mutable_flags()->clear_input();
    incoming->mutable_flags()->clear_deps_file();
    incoming->mutable_flags()->mutable_compiler()->clear_path();
    auto& plugins =
        *incoming->mutable_flags()->mutable_compiler()->mutable_plugins();
    for (auto& plugin : plugins) {
      plugin.clear_path();
    }

    HandledHash local_hash =
        GenerateHash(incoming->flags(), HandledSource(source), extra_files);

    Universal outgoing(new net::proto::Universal);

    cache::FileCache::Entry entry;
    if (SearchSimpleCache(local_hash, &entry)) {
      auto* result = outgoing->MutableExtension(proto::Result::extension);

      result->set_obj(entry.object);
      result->set_from_cache(true);
      if (incoming->has_handled_hash()) {
        auto remote_hash = Immutable::WrapString(incoming->handled_hash());
        result->set_hash_match(HandledHash(remote_hash) == local_hash);
      }

      auto status = outgoing->MutableExtension(net::proto::Status::extension);
      status->set_code(net::proto::Status::OK);
      status->set_description(entry.stderr);

      task->first->SendAsync(std::move(outgoing));
      continue;
    }

    // Optimize compilation for preprocessed code for some languages.
    if (incoming->flags().has_language()) {
      if (incoming->flags().language() == "c") {
        incoming->mutable_flags()->set_language("cpp-output");
      } else if (incoming->flags().language() == "c++") {
        incoming->mutable_flags()->set_language("c++-cpp-output");
      } else if (incoming->flags().language() == "objective-c++") {
        incoming->mutable_flags()->set_language("objective-c++-cpp-output");
      }
    }

    // Check that we have a compiler of a requested version.
    net::proto::Status status;
    if (!SetupCompiler(incoming->mutable_flags(), &status)) {
      task->first->ReportStatus(status);
      continue;
    }

    base::TemporaryDir temp_dir;
    if (!PrepareExtraFilesForCompiler(extra_files, temp_dir.GetPath(),
                                      incoming->mutable_flags(), &status)) {
      task->first->ReportStatus(status);
      continue;
    }

    // Pipe the input file to the compiler and read output file from the
    // compiler's stdout.
    String error;
    base::ProcessPtr process = CreateProcess(incoming->flags());
    if (!process->Run(conf()->absorber().run_timeout(), source, &error)) {
      status.set_code(net::proto::Status::EXECUTION);
      if (!process->stdout().empty() || !process->stderr().empty()) {
        status.set_description(process->stderr());
        LOG(WARNING) << "Compilation failed with error:" << std::endl
                     << process->stderr() << std::endl
                     << process->stdout();
      } else if (!error.empty()) {
        status.set_description(error);
        LOG(WARNING) << "Compilation failed with error: " << error;
      } else {
        status.set_description("without errors");
        LOG(WARNING) << "Compilation failed without errors";
      }

      // We lose atomicity, but the WARNING level will be less verbose.
      LOG(VERBOSE) << static_cast<const google::protobuf::Message&>(
          incoming->flags());
    } else {
      status.set_code(net::proto::Status::OK);
      status.set_description(process->stderr());
      LOG(INFO) << "External compilation successful";

      auto* result = outgoing->MutableExtension(proto::Result::extension);
      result->set_obj(process->stdout());
      result->set_from_cache(false);
      if (incoming->has_handled_hash()) {
        auto remote_hash = Immutable::WrapString(incoming->handled_hash());
        result->set_hash_match(HandledHash(remote_hash) == local_hash);
      }
    }

    outgoing->MutableExtension(net::proto::Status::extension)->CopyFrom(status);

    if (status.code() == net::proto::Status::OK) {
      cache::FileCache::Entry entry;

      entry.object = process->stdout();
      entry.stderr = Immutable(status.description());

      UpdateSimpleCache(local_hash, entry);
    }

    task->first->SendAsync(std::move(outgoing));
  }
}

}  // namespace daemon
}  // namespace dist_clang
