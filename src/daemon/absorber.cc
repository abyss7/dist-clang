#include <daemon/absorber.h>

#include <base/file_utils.h>
#include <base/logging.h>
#include <net/connection.h>
#include <base/using_log.h>

using namespace std::placeholders;

namespace dist_clang {
namespace daemon {

Absorber::Absorber(const proto::Configuration& configuration)
    : BaseDaemon(configuration) {
  using Worker = base::WorkerPool::SimpleWorker;

  CHECK(conf_.has_absorber() && !conf_.absorber().local().disabled());

  workers_.reset(new base::WorkerPool);
  tasks_.reset(new Queue(conf_.pool_capacity()));

  {
    Worker worker = std::bind(&Absorber::DoExecute, this, _1);
    workers_->AddWorker(worker, conf_.absorber().local().threads());
  }
}

Absorber::~Absorber() {
  tasks_->Close();
  workers_.reset();
}

bool Absorber::Initialize() {
  String error;
  const auto& local = conf_.absorber().local();
  if (!Listen(local.host(), local.port(), &error)) {
    LOG(ERROR) << "Failed to listen on " << local.host() << ":" << local.port()
               << " : " << error;
    return false;
  }

  return BaseDaemon::Initialize();
}

bool Absorber::HandleNewMessage(net::ConnectionPtr connection,
                                Universal message,
                                const proto::Status& status) {
  if (!message->IsInitialized()) {
    LOG(INFO) << message->InitializationErrorString();
    return false;
  }

  if (status.code() != proto::Status::OK) {
    LOG(ERROR) << status.description();
    return connection->ReportStatus(status);
  }

  if (message->HasExtension(proto::RemoteExecute::extension)) {
    Message execute(message->ReleaseExtension(proto::RemoteExecute::extension));
    if (execute->has_source()) {
      return tasks_->Push(Task{connection, std::move(execute)});
    }
  }

  NOTREACHED();
  return false;
}

void Absorber::DoExecute(const std::atomic<bool>& is_shutting_down) {
  while (!is_shutting_down) {
    Optional&& task = tasks_->Pop();
    if (!task) {
      break;
    }
    proto::RemoteExecute* incoming = task->second.get();

    FileCache::Entry cache_entry;
    if (SearchCache(incoming->flags(), incoming->source(), &cache_entry)) {
      Universal outgoing(new proto::Universal);
      auto* result = outgoing->MutableExtension(proto::RemoteResult::extension);
      result->set_obj(cache_entry.object);

      auto status = outgoing->MutableExtension(proto::Status::extension);
      status->set_code(proto::Status::OK);
      status->set_description(cache_entry.stderr);

      task->first->SendAsync(std::move(outgoing));
      continue;
    }

    // Check that we have a compiler of a requested version.
    proto::Status status;
    if (!SetupCompiler(incoming->mutable_flags(), &status)) {
      task->first->ReportStatus(status);
      continue;
    }

    incoming->mutable_flags()->set_output("-");
    incoming->mutable_flags()->clear_input();
    incoming->mutable_flags()->clear_deps_file();

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

    // Pipe the input file to the compiler and read output file from the
    // compiler's stdout.
    String error;
    base::ProcessPtr process = CreateProcess(incoming->flags());
    if (!process->Run(10, incoming->source(), &error)) {
      status.set_code(proto::Status::EXECUTION);
      if (!process->stdout().empty() || !process->stderr().empty()) {
        status.set_description(process->stderr());
        LOG(WARNING) << "Compilation failed with error:" << std::endl
                     << process->stderr() << std::endl << process->stdout();
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
      status.set_code(proto::Status::OK);
      status.set_description(process->stderr());
      LOG(INFO) << "External compilation successful";
    }

    Universal outgoing(new proto::Universal);
    const auto& result = proto::RemoteResult::extension;
    outgoing->MutableExtension(proto::Status::extension)->CopyFrom(status);
    outgoing->MutableExtension(result)->set_obj(process->stdout());

    if (status.code() == proto::Status::OK) {
      FileCache::Entry entry;

      entry.object = process->stdout();
      entry.stderr = status.description();

      UpdateCache(incoming->flags(), incoming->source(), entry);
    }

    task->first->SendAsync(std::move(outgoing));
  }
}

}  // namespace daemon
}  // namespace dist_clang
