#include <daemon/absorber.h>

#include <base/file_utils.h>
#include <base/logging.h>
#include <base/process.h>
#include <base/protobuf_utils.h>
#include <net/connection.h>

#include <base/using_log.h>

using namespace std::placeholders;

namespace dist_clang {
namespace daemon {

Absorber::Absorber(const proto::Configuration& configuration)
    : CompilationDaemon(configuration) {
  using Worker = base::WorkerPool::SimpleWorker;
  auto config = conf();
  CHECK(config->has_absorber() && !config->absorber().local().disabled());

  workers_.reset(new base::WorkerPool);
  tasks_.reset(new Queue(config->pool_capacity()));

  {
    Worker worker = std::bind(&Absorber::DoExecute, this, _1);
    workers_->AddWorker("Execute Worker"_l, worker,
                        config->absorber().local().threads());
  }
}

Absorber::~Absorber() {
  tasks_->Close();
  workers_.reset();
}

bool Absorber::Initialize() {
  String error;
  auto config = conf();
  const auto& local = config->absorber().local();
  if (!Listen(local.host(), local.port(), local.ipv6(), &error)) {
    LOG(ERROR) << "Failed to listen on " << local.host() << ":" << local.port()
               << " : " << error;
    return false;
  }

  if (config->has_cache() && config->cache().has_direct()) {
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
    auto config = conf();
    if (execute->emitter_version() < config->absorber().min_emitter_version()) {
      LOG(WARNING) << "Emitter sent a bad version: "
                   << execute->emitter_version();
      net::proto::Status bad_version_status;
      bad_version_status.set_code(net::proto::Status::BAD_EMITTER_VERSION);
      return connection->ReportStatus(bad_version_status);
    }

    DCHECK(!execute->flags().compiler().has_path());
    if (execute->has_source()) {
      return tasks_->Push(Task{connection, std::move(execute)});
    }
  }

  NOTREACHED();
  return false;
}

void Absorber::DoExecute(const Atomic<bool>& is_shutting_down) {
  using namespace cache::string;

  while (!is_shutting_down) {
    Optional&& task = tasks_->Pop();
    if (!task) {
      break;
    }

    if (task->first->IsClosed()) {
      continue;
    }

    proto::Remote* incoming = task->second.get();
    auto source = Immutable::WrapString(incoming->source());

    incoming->mutable_flags()->set_output("-");
    incoming->mutable_flags()->clear_input();
    incoming->mutable_flags()->clear_deps_file();
    incoming->mutable_flags()->mutable_compiler()->clear_path();
    auto& plugins =
        *incoming->mutable_flags()->mutable_compiler()->mutable_plugins();
    for (auto& plugin : plugins) {
      plugin.clear_path();
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

    cache::FileCache::Entry entry;
    if (SearchSimpleCache(incoming->flags(), HandledSource(source), &entry)) {
      Universal outgoing(new net::proto::Universal);
      auto* result = outgoing->MutableExtension(proto::Result::extension);
      result->set_obj(entry.object);

      auto status = outgoing->MutableExtension(net::proto::Status::extension);
      status->set_code(net::proto::Status::OK);
      status->set_description(entry.stderr);

      task->first->SendAsync(std::move(outgoing));
      continue;
    }

    // Check that we have a compiler of a requested version.
    net::proto::Status status;
    if (!SetupCompiler(incoming->mutable_flags(), &status)) {
      task->first->ReportStatus(status);
      continue;
    }

    Universal outgoing(new net::proto::Universal);

    // Pipe the input file to the compiler and read output file from the
    // compiler's stdout.
    String error;
    base::ProcessPtr process = CreateProcess(incoming->flags());
    if (!process->Run(30, source, &error)) {
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

      const auto& result = proto::Result::extension;
      outgoing->MutableExtension(result)->set_obj(process->stdout());
    }

    outgoing->MutableExtension(net::proto::Status::extension)->CopyFrom(status);

    if (status.code() == net::proto::Status::OK) {
      cache::FileCache::Entry entry;

      entry.object = process->stdout();
      entry.stderr = Immutable(status.description());

      UpdateSimpleCache(incoming->flags(), HandledSource(source), entry);
    }

    task->first->SendAsync(std::move(outgoing));
  }
}

}  // namespace daemon
}  // namespace dist_clang
