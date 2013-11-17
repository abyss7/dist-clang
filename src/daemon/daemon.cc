#include "daemon/daemon.h"

#include "base/assert.h"
#include "base/file_utils.h"
#include "base/logging.h"
#include "base/process.h"
#include "base/queue_aggregator_impl.h"
#include "base/string_utils.h"
#include "base/worker_pool.h"
#include "daemon/configuration.h"
#include "daemon/statistic.h"
#include "net/base/end_point.h"
#include "net/connection.h"
#include "net/network_service.h"
#include "proto/config.pb.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#if defined(PROFILER)
#  include <gperftools/profiler.h>
#endif
#include <mutex>

#include "base/using_log.h"

using namespace std::placeholders;

namespace dist_clang {
namespace daemon {

#if defined(PROFILER)
Daemon::Daemon() {
  ProfilerStart("clangd.prof");
}
#endif  // PROFILER

Daemon::~Daemon() {
#if defined(PROFILER)
  ProfilerStop();
#endif  // PROFILER

  cache_tasks_->Close();
  all_tasks_->Close();
  local_tasks_->Close();
  failed_tasks_->Close();
  remote_tasks_->Close();
  workers_.reset();
  network_service_.reset();
}

bool Daemon::Initialize(const Configuration &configuration) {
  using Worker = base::WorkerPool::SimpleWorker;

  const auto& config = configuration.config();

  if (!config.IsInitialized()) {
    LOG(ERROR) << config.InitializationErrorString();
    return false;
  }

  network_service_.reset(new net::NetworkService);
  if (config.has_statistic()) {
    // TODO: initialize statistic in proper way - regarding |NetworkService|.
    // Statistic::Initialize(network_service_, config.statistic());
  }

  workers_.reset(new base::WorkerPool);
  all_tasks_.reset(new QueueAggregator);
  local_tasks_.reset(new Queue);
  failed_tasks_.reset(new Queue);
  if (config.has_local()) {
    remote_tasks_.reset(new Queue(config.pool_capacity()));
    Worker worker = std::bind(&Daemon::DoLocalExecution, this, _1);
    workers_->AddWorker(worker, config.local().threads());
  }
  else {
    remote_tasks_.reset(new Queue);
    Worker worker = std::bind(&Daemon::DoLocalExecution, this, _1);
    workers_->AddWorker(worker, std::thread::hardware_concurrency());
  }
  // FIXME: need to improve |QueueAggregator| to prioritize queues.
  all_tasks_->Aggregate(local_tasks_.get());
  all_tasks_->Aggregate(failed_tasks_.get());
  all_tasks_->Aggregate(remote_tasks_.get());

  cache_tasks_.reset(new Queue);
  if (config.has_cache_path()) {
    cache_.reset(new FileCache(config.cache_path()));
    Worker worker = std::bind(&Daemon::DoCheckCache, this, _1);
    workers_->AddWorker(worker, std::thread::hardware_concurrency());
  }

  bool is_listening = false;
  if (config.has_local() && !config.local().disabled()) {
    std::string error;
    auto callback = std::bind(&Daemon::HandleNewConnection, this, _1);
    bool result = network_service_->Listen(config.local().host(),
                                           config.local().port(),
                                           callback, &error);
    if (!result) {
      LOG(ERROR) << "Failed to listen on " << config.local().host() << ":"
                 << config.local().port() << " : " << error;
    }
    is_listening |= result;
  }

  if (config.has_socket_path()) {
    auto callback = std::bind(&Daemon::HandleNewConnection, this, _1);
    is_listening |= network_service_->Listen(config.socket_path(), callback);
  }

  if (!is_listening) {
    LOG(ERROR) << "Daemon is not listening.";
    return false;
  }

  for (const auto& remote: config.remotes()) {
    if (!remote.disabled()) {
      auto end_point = net::EndPoint::TcpHost(remote.host(), remote.port());
      Worker worker =
          std::bind(&Daemon::DoRemoteExecution, this, _1, end_point);
      workers_->AddWorker(worker, remote.threads());
    }
  }

  for (const auto& version: config.versions()) {
    compilers_.insert(std::make_pair(version.version(), version.path()));

    // Load plugins.
    auto value = std::make_pair(version.version(), PluginNameMap());
    auto& plugin_map = plugins_.insert(value).first->second;
    for (const auto& plugin: version.plugins()) {
      plugin_map.insert(std::make_pair(plugin.name(), plugin.path()));
    }
  }

  base::Log::RangeSet ranges;
  for (const auto& level: config.verbosity().levels()) {
    if (level.has_left() && level.left() > level.right()) {
      continue;
    }

    if (!level.has_left()) {
      ranges.insert(std::make_pair(level.right(), level.right()));
    }
    else {
      ranges.insert(std::make_pair(level.left(), level.right()));
    }
  }

  base::Log::RangeSet results;
  if (!ranges.empty()) {
    auto current = *ranges.begin();
    if (ranges.size() > 1) {
      for (auto it = std::next(ranges.begin()); it != ranges.end(); ++it) {
        if (current.second + 1 >= it->first) {
          current.second = std::max(it->second, current.second);
        }
        else {
          results.insert(std::make_pair(current.second, current.first));
          current = *it;
        }
      }
    }
    results.insert(std::make_pair(current.second, current.first));
  }
  base::Log::Init(config.verbosity().error_mark(), std::move(results));

  return network_service_->Run();
}

bool Daemon::SearchCache(const proto::Execute* message,
                         FileCache::Entry *entry) {
  if (!cache_ || !message || !entry) {
    return false;
  }

  const auto& flags = message->cc_flags();
  const auto& version = flags.compiler().version();
  std::string command_line = base::JoinString<' '>(flags.other().begin(),
                                                   flags.other().end());
  if (flags.has_language()) {
    command_line += " -x " + flags.language();
  }
  return cache_->Find(message->pp_source(), command_line, version, entry);
}

void Daemon::UpdateCache(const proto::Execute* message,
                         const proto::Status& status) {
  if (!cache_ || !message || !message->has_pp_source()) {
    return;
  }
  CHECK(status.code() == proto::Status::OK);

  const auto& flags = message->cc_flags();
  FileCache::Entry entry;
  entry.first = message->current_dir() + "/" + flags.output();
  entry.second = status.description();
  const auto& version = flags.compiler().version();
  std::string command_line = base::JoinString<' '>(flags.other().begin(),
                                                   flags.other().end());
  if (flags.has_language()) {
    command_line += " -x " + flags.language();
  }
  cache_->Store(message->pp_source(), command_line, version, entry);
}

bool Daemon::FillFlags(proto::Flags* flags, proto::Status* status) {
  // No flags - filled flags.
  if (!flags) {
    return true;
  }

  if (!flags->compiler().has_path()) {
    auto compiler = compilers_.find(flags->compiler().version());
    if (compiler == compilers_.end()) {
      if (status) {
        status->set_code(proto::Status::NO_VERSION);
        status->set_description("Compiler of the required version not found");
      }
      return false;
    }
    flags->mutable_compiler()->set_path(compiler->second);
  }

  auto plugin_map = plugins_.find(flags->compiler().version());
  auto& plugins = *flags->mutable_compiler()->mutable_plugins();
  for (auto& plugin: plugins) {
    if (!plugin.has_path() && plugin_map == plugins_.end()) {
      if (status) {
        status->set_code(proto::Status::NO_VERSION);
        status->set_description("Plugin " + plugin.name() + " not found");
      }
      return false;
    }
    auto plugin_by_name = plugin_map->second.find(plugin.name());
    if (plugin_by_name == plugin_map->second.end()) {
      if (status) {
        status->set_code(proto::Status::NO_VERSION);
        status->set_description("Plugin " + plugin.name() + " not found");
      }
      return false;
    }
    plugin.set_path(plugin_by_name->second);
  }

  return true;
}

void Daemon::HandleNewConnection(net::ConnectionPtr connection) {
  auto callback = std::bind(&Daemon::HandleNewMessage, this, _1, _2, _3);
  connection->ReadAsync(callback);
}

bool Daemon::HandleNewMessage(net::ConnectionPtr connection,
                              ScopedMessage message,
                              const proto::Status& status) {
  if (status.code() != proto::Status::OK) {
    LOG(ERROR) << status.description();
    return connection->ReportStatus(status);
  }

  if (!message->HasExtension(proto::Execute::extension) ||
      !message->GetExtension(proto::Execute::extension).has_remote()) {
    NOTREACHED();
    return false;
  }
  auto* extension = message->ReleaseExtension(proto::Execute::extension);
  std::unique_ptr<proto::Execute> execute(extension);

  if (cache_) {
    if (( execute->remote() && execute->has_pp_source()) ||
        (!execute->remote() && execute->has_current_dir())) {
      return cache_tasks_->Push(std::make_pair(connection, std::move(execute)));
    }
  }
  else if (execute->remote() && execute->has_pp_source()) {
    return remote_tasks_->Push(std::make_pair(connection, std::move(execute)));
  }
  else if (!execute->remote() && execute->has_current_dir()) {
    return local_tasks_->Push(std::make_pair(connection, std::move(execute)));
  }

  NOTREACHED();
  return false;
}

void Daemon::DoCheckCache(const std::atomic<bool>& is_shutting_down) {
  while (!is_shutting_down) {
    ScopedTask task;
    if (!cache_tasks_->Pop(task)) {
      break;
    }
    auto* message = task.second.get();

    if (!message->has_pp_source()) {
      if (!message->has_pp_flags() ||
          !FillFlags(message->mutable_pp_flags())) {
        // Without preprocessing flags we can't neither check the cache, nor do
        // a remote compilation, nor put the result back in cache.
        failed_tasks_->Push(std::move(task));
        continue;
      }

      // Redirect output to stdin in |pp_flags|.
      message->mutable_pp_flags()->set_output("-");

      base::Process process(message->pp_flags(), message->current_dir());
      if (!process.Run(10)) {
        // It usually means, that there is an error in the source code.
        // We should skip a cache check and head to local compilation.
        failed_tasks_->Push(std::move(task));
        continue;
      }
      message->set_pp_source(process.stdout());
    }

    FileCache::Entry cache_entry;
    if (SearchCache(message, &cache_entry)) {
      if (!message->remote()) {
        std::string output_path =
            message->current_dir() + "/" + message->cc_flags().output();
        if (base::CopyFile(cache_entry.first, output_path, true)) {
          proto::Status status;
          status.set_code(proto::Status::OK);
          status.set_description(cache_entry.second);
          task.first->ReportStatus(status);
          continue;
        }
      }
      else {
        Daemon::ScopedMessage message(new proto::Universal);
        auto result = message->MutableExtension(proto::RemoteResult::extension);
        if (base::ReadFile(cache_entry.first, result->mutable_obj())) {
          auto status = message->MutableExtension(proto::Status::extension);
          status->set_code(proto::Status::OK);
          status->set_description(cache_entry.second);
          task.first->SendAsync(std::move(message));
          continue;
        }
      }
    }

    if (!message->remote()) {
      local_tasks_->Push(std::move(task));
    }
    else {
      remote_tasks_->Push(std::move(task));
    }
  }
}

void Daemon::DoRemoteExecution(const std::atomic<bool>& is_shutting_down,
                               net::EndPointPtr end_point) {
  if (!end_point) {
    // TODO: do re-resolve |end_point| periodically, since the network
    // configuration may change on runtime.
    return;
  }

  while (!is_shutting_down) {
    ScopedTask task;
    if (!local_tasks_->Pop(task)) {
      break;
    }
    auto* message = task.second.get();

    if (!message->has_pp_source()) {
      if (!message->has_pp_flags() ||
          !FillFlags(message->mutable_pp_flags())) {
        // Without preprocessing flags we can't neither check the cache, nor do
        // a remote compilation, nor put the result back in cache.
        failed_tasks_->Push(std::move(task));
        continue;
      }

      // Redirect output to stdin in |pp_flags|.
      message->mutable_pp_flags()->set_output("-");

      base::Process process(message->pp_flags(), message->current_dir());
      if (!process.Run(10)) {
        // It usually means, that there is an error in the source code.
        // We should skip a cache check and head to local compilation.
        failed_tasks_->Push(std::move(task));
        continue;
      }
      message->set_pp_source(process.stdout());
    }

    // TODO: cache connection before popping a task.
    auto connection = network_service_->Connect(end_point);
    if (!connection) {
      local_tasks_->Push(std::move(task));
      continue;
    }

    ScopedExecute remote(new proto::Execute);
    remote->set_remote(true);
    remote->set_pp_source(message->pp_source());
    remote->mutable_cc_flags()->CopyFrom(message->cc_flags());

    // Filter outgoing flags.
    remote->mutable_cc_flags()->mutable_compiler()->clear_path();
    remote->mutable_cc_flags()->clear_output();
    remote->mutable_cc_flags()->clear_input();
    remote->mutable_cc_flags()->clear_dependenies();

    if (!connection->SendSync(std::move(remote))) {
      local_tasks_->Push(std::move(task));
      continue;
    }

    ScopedMessage result(new proto::Universal);
    if (!connection->ReadSync(result.get())) {
      local_tasks_->Push(std::move(task));
      continue;
    }

    if (result->HasExtension(proto::Status::extension)) {
      const auto& status = result->GetExtension(proto::Status::extension);
      if (status.code() != proto::Status::OK) {
        LOG(WARNING) << "Remote compilation failed with error(s):" << std::endl
                     << status.description();
        failed_tasks_->Push(std::move(task));
        continue;
      }
    }

    if (result->HasExtension(proto::RemoteResult::extension)) {
      const auto& extension =
          result->GetExtension(proto::RemoteResult::extension);
      std::string output_path =
          message->current_dir() + "/" + message->cc_flags().output();
      if (base::WriteFile(output_path, extension.obj())) {
        proto::Status status;
        status.set_code(proto::Status::OK);
        LOG(INFO) << "Remote compilation successful: "
                  << message->cc_flags().input();
        UpdateCache(message, status);
        task.first->ReportStatus(status);
        continue;
      }
    }

    // In case this task has crashed the remote end, we will try only local
    // compilation next time.
    failed_tasks_->Push(std::move(task));
  }
}

void Daemon::DoLocalExecution(const std::atomic<bool>& is_shutting_down) {
  while (!is_shutting_down) {
    ScopedTask task;
    if (!all_tasks_->Pop(task)) {
      break;
    }

    if (!task.second->remote()) {
      proto::Status status;
      if (!FillFlags(task.second->mutable_cc_flags(), &status)) {
        task.first->ReportStatus(status);
        continue;
      }

      std::string error;
      base::Process process(task.second->cc_flags(),
                            task.second->current_dir());
      if (!process.Run(base::Process::UNLIMITED, &error)) {
        status.set_code(proto::Status::EXECUTION);
        if (!process.stderr().empty()) {
          status.set_description(process.stderr());
        }
        else if (!error.empty()) {
          status.set_description(error);
        }
        else {
          status.set_description("without errors");
        }
      } else {
        status.set_code(proto::Status::OK);
        status.set_description(process.stderr());
        LOG(INFO) << "Local compilation successful:  "
                  << task.second->cc_flags().input();
        UpdateCache(task.second.get(), status);
      }

      task.first->ReportStatus(status);
    }
    else {
      // Check that we have a compiler of a requested version.
      proto::Status status;
      if (!FillFlags(task.second->mutable_cc_flags(), &status)) {
        task.first->ReportStatus(status);
        continue;
      }

      task.second->mutable_cc_flags()->set_output("-");
      task.second->mutable_cc_flags()->clear_input();
      task.second->mutable_cc_flags()->clear_dependenies();

      // Optimize compilation for preprocessed code for some languages.
      if (task.second->cc_flags().has_language()) {
        if (task.second->cc_flags().language() == "c") {
          task.second->mutable_cc_flags()->set_language("cpp-output");
        }
        else if (task.second->cc_flags().language() == "c++") {
          task.second->mutable_cc_flags()->set_language("c++-cpp-output");
        }
      }

      // Do local compilation. Pipe the input file to the compiler and read
      // output file from the compiler's stdout.
      std::string error;
      base::Process process(task.second->cc_flags());
      if (!process.Run(10, task.second->pp_source(), &error)) {
        std::stringstream arguments;
        arguments << "Arguments:";
        for (const auto& flag: task.second->cc_flags().other()) {
          arguments << " " << flag;
        }
        arguments << std::endl << std::endl;

        status.set_code(proto::Status::EXECUTION);
        if (!process.stdout().empty() || !process.stderr().empty()) {
          status.set_description(process.stderr());
          LOG(WARNING) << "Compilation failed with error:" << std::endl
                       << process.stderr() << std::endl
                       << process.stdout() << std::endl
                       << arguments.str();
        }
        else if (!error.empty()) {
          status.set_description(error);
          LOG(WARNING) << "Compilation failed with error: " << error
                       << std::endl << arguments.str();
        }
        else {
          status.set_description("without errors");
          LOG(WARNING) << "Compilation failed without errors" << std::endl
                       << arguments.str();
        }
      }
      else {
        status.set_code(proto::Status::OK);
        status.set_description(process.stderr());
        LOG(INFO) << "External compilation successful";
      }

      Daemon::ScopedMessage message(new proto::Universal);
      const auto& result = proto::RemoteResult::extension;
      message->MutableExtension(proto::Status::extension)->CopyFrom(status);
      message->MutableExtension(result)->set_obj(process.stdout());
      task.first->SendAsync(std::move(message));
    }
  }
}

}  // namespace daemon
}  // namespace dist_clang
