#include <daemon/daemon.h>

#include <base/assert.h>
#include <base/file_utils.h>
#include <base/logging.h>
#include <base/process_impl.h>
#include <base/string_utils.h>
#include <base/worker_pool.h>
#include <daemon/configuration.pb.h>
#include <daemon/statistic.h>
#include <net/base/end_point.h>
#include <net/connection.h>
#include <net/network_service_impl.h>

#include <third_party/libcxx/exported/include/atomic>
#include <third_party/libcxx/exported/include/condition_variable>
#if defined(PROFILER)
#include <gperftools/profiler.h>
#endif
#include <third_party/libcxx/exported/include/mutex>

#include <base/using_log.h>

using namespace std::placeholders;

namespace dist_clang {

namespace {

inline String CommandLineForCache(const dist_clang::proto::Flags& flags) {
  String command_line =
      base::JoinString<' '>(flags.other().begin(), flags.other().end());
  if (flags.has_language()) {
    command_line += " -x " + flags.language();
  }
  if (flags.cc_only_size()) {
    command_line += " " + base::JoinString<' '>(flags.cc_only().begin(),
                                                flags.cc_only().end());
  }

  return command_line;
}

inline String CommandLineForDirect(const dist_clang::proto::Flags& flags) {
  String command_line =
      base::JoinString<' '>(flags.other().begin(), flags.other().end());
  if (flags.has_language()) {
    command_line += " -x " + flags.language();
  }
  if (flags.non_cached_size()) {
    command_line += " " + base::JoinString<' '>(flags.non_cached().begin(),
                                                flags.non_cached().end());
  }
  if (flags.cc_only_size()) {
    command_line += " " + base::JoinString<' '>(flags.cc_only().begin(),
                                                flags.cc_only().end());
  }

  return command_line;
}

bool ParseDepsFile(const String& path, const String& base_path,
                   List<String>& headers) {
  String contents;
  if (!base::ReadFile(path, &contents)) {
    return false;
  }

  List<String> lines;
  base::SplitString<'\n'>(contents, lines);
  String last_line = lines.back();
  lines.pop_front();
  if (lines.empty()) {
    return true;
  }
  lines.pop_back();
  for (const auto& line : lines) {
    base::SplitString<' '>(line.substr(2, line.size() - 4), headers);
  }
  base::SplitString<' '>(last_line.substr(2, last_line.size() - 2), headers);

  for (auto& header : headers) {
    if (header[0] != '/') {
      header = base_path + "/" + header;
    }
  }

  return true;
}

}  // namespace

namespace daemon {

#if defined(PROFILER)
Daemon::Daemon() { ProfilerStart("clangd.prof"); }
#endif  // PROFILER

Daemon::~Daemon() {
#if defined(PROFILER)
  ProfilerStop();
#endif  // PROFILER

  if (cache_tasks_) {
    cache_tasks_->Close();
  }
  if (local_tasks_) {
    local_tasks_->Close();
  }
  if (failed_tasks_) {
    failed_tasks_->Close();
  }
  if (remote_tasks_) {
    remote_tasks_->Close();
  }
  if (all_tasks_) {
    all_tasks_->Close();
  }

  workers_.reset();
  network_service_.reset();
  stat_service_.reset();
}

bool Daemon::Initialize(const Configuration& configuration) {
  using Worker = base::WorkerPool::SimpleWorker;

  const auto& config = configuration.config();

  if (!config.IsInitialized()) {
    LOG(ERROR) << config.InitializationErrorString();
    return false;
  }

  network_service_ = net::NetworkService::Create();
  if (config.has_statistic() && !config.statistic().disabled()) {
    stat_service_ = daemon::Statistic::Create();
    if (!stat_service_->Initialize(*network_service_, config.statistic())) {
      LOG(ERROR) << "Failed to start the statistic service";
    }
  }

  workers_.reset(new base::WorkerPool);
  all_tasks_.reset(new QueueAggregator);
  local_tasks_.reset(new Queue);
  failed_tasks_.reset(new Queue);
  if (config.has_local()) {
    remote_tasks_.reset(new Queue(config.pool_capacity()));
    Worker worker = std::bind(&Daemon::DoLocalExecution, this, _1);
    workers_->AddWorker(worker, config.local().threads());
  } else {
    // |remote_tasks_| will be always empty, since the daemon doesn't listen for
    // remote connections.
    remote_tasks_.reset(new Queue);
    Worker worker = std::bind(&Daemon::DoLocalExecution, this, _1);
    workers_->AddWorker(worker, std::thread::hardware_concurrency() * 2);
  }
  // FIXME: need to improve |QueueAggregator| to prioritize queues.
  if (!config.has_local() || !config.local().only_failed()) {
    all_tasks_->Aggregate(local_tasks_.get());
  }
  all_tasks_->Aggregate(failed_tasks_.get());
  all_tasks_->Aggregate(remote_tasks_.get());

  cache_tasks_.reset(new Queue);
  if (config.has_cache()) {
    cache_.reset(new FileCache(config.cache().path(), config.cache().size()));
    Worker worker = std::bind(&Daemon::DoCheckCache, this, _1);
    workers_->AddWorker(worker, std::thread::hardware_concurrency() * 2);
    cache_config_.reset(config.cache().New());
    cache_config_->CopyFrom(config.cache());
  }

  bool is_listening = false;
  if (config.has_local() && !config.local().disabled()) {
    String error;
    auto callback = std::bind(&Daemon::HandleNewConnection, this, _1);
    if (!network_service_->Listen(config.local().host(), config.local().port(),
                                  callback, &error)) {
      LOG(ERROR) << "Failed to listen on " << config.local().host() << ":"
                 << config.local().port() << " : " << error;
    } else {
      is_listening = true;
    }
  }

  if (config.has_socket_path()) {
    String error;
    auto callback = std::bind(&Daemon::HandleNewConnection, this, _1);
    if (!network_service_->Listen(config.socket_path(), callback, &error)) {
      LOG(ERROR) << "Failed to listen on " << config.socket_path() << " : "
                 << error;
    } else {
      is_listening = true;
    }
  }

  if (!is_listening) {
    LOG(ERROR) << "Daemon is not listening.";
    return false;
  }

  for (const auto& remote : config.remotes()) {
    if (!remote.disabled()) {
      auto end_point = net::EndPoint::TcpHost(remote.host(), remote.port());
      Worker worker =
          std::bind(&Daemon::DoRemoteExecution, this, _1, end_point);
      workers_->AddWorker(worker, remote.threads());
    }
  }

  for (const auto& version : config.versions()) {
    if (!version.has_path() || version.path().empty()) {
      LOG(ERROR) << "Compiler " << version.version() << " has no path.";
      return false;
    }
    compilers_.insert(std::make_pair(version.version(), version.path()));

    // Load plugins.
    auto value = std::make_pair(version.version(), PluginNameMap());
    auto& plugin_map = plugins_.insert(value).first->second;
    for (const auto& plugin : version.plugins()) {
      if (!plugin.has_path() || plugin.path().empty()) {
        LOG(ERROR) << "Plugin " << plugin.name() << " for compiler "
                   << version.version() << " has no path.";
        return false;
      }
      plugin_map.insert(std::make_pair(plugin.name(), plugin.path()));
    }
  }

  if (config.has_verbosity()) {
    base::Log::RangeSet ranges;
    for (const auto& level : config.verbosity().levels()) {
      if (level.has_left() && level.left() > level.right()) {
        continue;
      }

      if (!level.has_left()) {
        ranges.insert(std::make_pair(level.right(), level.right()));
      } else {
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
          } else {
            results.insert(std::make_pair(current.second, current.first));
            current = *it;
          }
        }
      }
      results.insert(std::make_pair(current.second, current.first));
    }
    base::Log::Reset(config.verbosity().error_mark(), std::move(results));
  }

  return network_service_->Run();
}

bool Daemon::SearchCache(const proto::Execute* message,
                         FileCache::Entry* entry) {
  if (!cache_ || !message || !entry) {
    LOG(CACHE_WARNING) << "Failed to check the cache";
    return false;
  }

  const auto& flags = message->flags();
  const auto& version = flags.compiler().version();
  const String command_line = CommandLineForCache(flags);

  if (!cache_->Find(message->pp_source(), command_line, version, entry)) {
    LOG(CACHE_INFO) << "Cache miss: " << flags.input();
    return false;
  }
  return true;
}

bool Daemon::SearchDirectCache(const proto::Execute* message,
                               FileCache::Entry* entry) {
  if (!cache_ || !cache_config_->direct() || !message || !entry ||
      message->remote()) {
    LOG(CACHE_WARNING) << "Failed to check the direct cache";
    return false;
  }

  const auto& flags = message->flags();
  const auto& version = flags.compiler().version();
  const String input_path = message->current_dir() + "/" + flags.input();
  const String command_line = CommandLineForDirect(flags);

  String original_code;
  if (!base::ReadFile(input_path, &original_code)) {
    return false;
  }

  if (!cache_->Find_Direct(original_code, command_line, version, entry)) {
    LOG(CACHE_INFO) << "Direct cache miss: " << flags.input();
    return false;
  }
  return true;
}

void Daemon::UpdateCacheFromFlags(const proto::Execute* message,
                                  const proto::Status& status) {
  if (!cache_ || !message || !message->has_pp_source()) {
    LOG(CACHE_WARNING) << "Failed to update the cache";
    return;
  }
  CHECK(status.code() == proto::Status::OK);

  FileCache::Entry entry;
  const auto& flags = message->flags();

  entry.object_path = message->current_dir() + "/" + flags.output();
  if (flags.has_deps_file()) {
    entry.deps_path = message->current_dir() + "/" + flags.deps_file();
  }
  entry.stderr = status.description();

  UpdateCache(message, entry);
}

void Daemon::UpdateCacheFromRemote(const proto::Execute* message,
                                   const proto::RemoteResult& result,
                                   const proto::Status& status) {
  if (!cache_ || !message || !message->has_pp_source()) {
    LOG(CACHE_WARNING) << "Failed to update the cache";
    return;
  }
  CHECK(status.code() == proto::Status::OK);

  String error;
  FileCache::Entry entry;

  if (result.has_obj()) {
    entry.object_path = base::CreateTempFile(&error);
    entry.move_object = true;
    if (entry.object_path.empty()) {
      LOG(CACHE_ERROR) << "Failed to create temporary file: " << error;
      return;
    }

    if (!base::WriteFile(entry.object_path, result.obj())) {
      LOG(CACHE_ERROR) << "Failed to write object to " << entry.object_path;
      return;
    }
  }
  if (result.has_deps()) {
    entry.deps_path = base::CreateTempFile(&error);
    entry.move_deps = true;
    if (entry.deps_path.empty()) {
      LOG(CACHE_ERROR) << "Failed to create temporary file: " << error;
      return;
    }

    if (!base::WriteFile(entry.deps_path, result.deps())) {
      LOG(CACHE_ERROR) << "Failed to write deps to " << entry.deps_path;
      return;
    }
  } else if (message->flags().has_deps_file()) {
    entry.deps_path =
        message->current_dir() + "/" + message->flags().deps_file();
  }
  entry.stderr = status.description();

  UpdateCache(message, entry);
}

void Daemon::UpdateCache(const proto::Execute* message,
                         const FileCache::Entry& entry) {
  const auto& flags = message->flags();
  const auto& version = flags.compiler().version();
  const String command_line = CommandLineForCache(flags);

  DCHECK(!!cache_config_);
  if (cache_config_->sync()) {
    cache_->StoreNow(message->pp_source(), command_line, version, entry);
  } else {
    cache_->Store(message->pp_source(), command_line, version, entry);
  }

  if (!message->remote() && !entry.deps_path.empty() &&
      cache_config_->direct()) {
    const auto hash = cache_->Hash(message->pp_source(), command_line, version);
    const String command_line = CommandLineForDirect(flags);
    const String input_path = message->current_dir() + "/" + flags.input();
    List<String> headers;
    String original_code;

    if (ParseDepsFile(entry.deps_path, message->current_dir(), headers) &&
        base::ReadFile(input_path, &original_code)) {
      cache_->Store_Direct(original_code, command_line, version, headers, hash);
    } else {
      LOG(CACHE_ERROR) << "Failed to parse deps file " << entry.deps_path
                       << " or read input " << input_path;
    }
  }
}

// static
proto::Flags Daemon::ConvertFlags(const proto::Flags& flags) {
  proto::Flags pp_flags;

  pp_flags.CopyFrom(flags);
  pp_flags.clear_cc_only();
  pp_flags.set_output("-");
  pp_flags.set_action("-E");

  return pp_flags;
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
  for (auto& plugin : plugins) {
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
  LOG(VERBOSE) << "New incoming connection";
}

bool Daemon::HandleNewMessage(net::ConnectionPtr connection,
                              ScopedMessage message,
                              const proto::Status& status) {
  message->CheckInitialized();
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
  UniquePtr<proto::Execute> execute(extension);

  if (cache_) {
    if ((execute->remote() && execute->has_pp_source()) ||
        (!execute->remote() && execute->has_current_dir())) {
      return cache_tasks_->Push(std::make_pair(connection, std::move(execute)));
    }
  } else if (execute->remote() && execute->has_pp_source()) {
    return remote_tasks_->Push(std::make_pair(connection, std::move(execute)));
  } else if (!execute->remote() && execute->has_current_dir()) {
    return local_tasks_->Push(std::make_pair(connection, std::move(execute)));
  }

  NOTREACHED();
  return false;
}

// static
base::ProcessPtr Daemon::CreateProcess(const proto::Flags& flags, ui32 uid,
                                       const String& cwd_path) {
  base::ProcessPtr process =
      base::Process::Create(flags.compiler().path(), cwd_path, uid);

  // |flags.other()| always must go first, since it contains the "-cc1" flag.
  process->AppendArg(flags.other().begin(), flags.other().end());
  process->AppendArg(flags.action());
  process->AppendArg(flags.non_cached().begin(), flags.non_cached().end());
  process->AppendArg(flags.non_direct().begin(), flags.non_direct().end());
  for (const auto& plugin : flags.compiler().plugins()) {
    process->AppendArg("-load").AppendArg(plugin.path());
  }
  if (flags.has_deps_file()) {
    process->AppendArg("-dependency-file").AppendArg(flags.deps_file());
  }
  if (flags.has_language()) {
    process->AppendArg("-x").AppendArg(flags.language());
  }
  if (flags.has_output()) {
    process->AppendArg("-o").AppendArg(flags.output());
  }
  if (flags.has_input()) {
    process->AppendArg(flags.input());
  }

  return std::move(process);
}

// static
base::ProcessPtr Daemon::CreateProcess(const proto::Flags& flags,
                                       const String& cwd_path) {
  return std::move(CreateProcess(flags, base::Process::SAME_UID, cwd_path));
}

void Daemon::DoCheckCache(const std::atomic<bool>& is_shutting_down) {
  while (!is_shutting_down) {
    Optional&& task = cache_tasks_->Pop();
    if (!task) {
      break;
    }
    auto* message = task->second.get();

    FileCache::Entry cache_entry;
    if (SearchDirectCache(message, &cache_entry)) {
      const String output_path =
          message->current_dir() + "/" + message->flags().output();
      if (base::CopyFile(cache_entry.object_path, output_path, true)) {
        String error;
        if (message->has_user_id() &&
            !base::ChangeOwner(output_path, message->user_id(), &error)) {
          LOG(ERROR) << "Failed to change owner for " << output_path << ": "
                     << error;
        }

        proto::Status status;
        status.set_code(proto::Status::OK);
        status.set_description(cache_entry.stderr);
        task->first->ReportStatus(status);
        continue;
      } else {
        LOG(ERROR) << "Failed to restore file from cache: " << output_path;
      }
    }

    if (!message->has_pp_source()) {
      proto::Flags&& pp_flags = ConvertFlags(message->flags());

      if (!FillFlags(&pp_flags)) {
        // Without preprocessing flags we can't neither check the cache, nor do
        // a remote compilation, nor put the result back in cache.
        failed_tasks_->Push(std::move(*task));
        continue;
      }

      base::ProcessPtr process =
          CreateProcess(pp_flags, message->current_dir());
      if (!process->Run(10)) {
        // It usually means, that there is an error in the source code.
        // We should skip a cache check and head to local compilation.
        failed_tasks_->Push(std::move(*task));
        continue;
      }
      message->set_pp_source(process->stdout());
    }

    if (SearchCache(message, &cache_entry)) {
      if (!message->remote()) {
        const String output_path =
            message->current_dir() + "/" + message->flags().output();
        if (base::CopyFile(cache_entry.object_path, output_path, true)) {
          String error;
          if (message->has_user_id() &&
              !base::ChangeOwner(output_path, message->user_id(), &error)) {
            LOG(ERROR) << "Failed to change owner for " << output_path << ": "
                       << error;
          }

          proto::Status status;
          status.set_code(proto::Status::OK);
          status.set_description(cache_entry.stderr);
          task->first->ReportStatus(status);
          continue;
        } else {
          LOG(ERROR) << "Failed to restore file from cache: " << output_path;
        }
      } else {
        Daemon::ScopedMessage message(new proto::Universal);
        auto result = message->MutableExtension(proto::RemoteResult::extension);
        if (base::ReadFile(cache_entry.object_path, result->mutable_obj())) {
          auto status = message->MutableExtension(proto::Status::extension);
          status->set_code(proto::Status::OK);
          status->set_description(cache_entry.stderr);
          task->first->SendAsync(std::move(message));
          continue;
        }
      }
    }

    if (!message->remote()) {
      local_tasks_->Push(std::move(*task));
    } else {
      remote_tasks_->Push(std::move(*task));
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
    Optional&& task = local_tasks_->Pop();
    if (!task) {
      break;
    }
    auto* message = task->second.get();

    if (!message->has_pp_source()) {
      proto::Flags&& pp_flags = ConvertFlags(message->flags());

      if (!FillFlags(&pp_flags)) {
        // Without preprocessing flags we can't neither check the cache, nor do
        // a remote compilation, nor put the result back in cache.
        failed_tasks_->Push(std::move(*task));
        continue;
      }

      base::ProcessPtr process =
          CreateProcess(pp_flags, message->current_dir());
      if (!process->Run(10)) {
        // It usually means, that there is an error in the source code.
        // We should skip a cache check and head to local compilation.
        failed_tasks_->Push(std::move(*task));
        continue;
      }
      message->set_pp_source(process->stdout());
    }

    // TODO: cache connection before popping a task.
    auto connection = network_service_->Connect(end_point);
    if (!connection) {
      local_tasks_->Push(std::move(*task));
      continue;
    }

    ScopedExecute remote(new proto::Execute);
    remote->set_remote(true);
    remote->set_pp_source(message->pp_source());
    remote->mutable_flags()->CopyFrom(message->flags());

    // Filter outgoing flags.
    remote->mutable_flags()->mutable_compiler()->clear_path();
    remote->mutable_flags()->clear_output();
    remote->mutable_flags()->clear_input();
    remote->mutable_flags()->clear_non_cached();
    remote->mutable_flags()->clear_deps_file();

    if (!connection->SendSync(std::move(remote))) {
      local_tasks_->Push(std::move(*task));
      continue;
    }

    ScopedMessage result(new proto::Universal);
    if (!connection->ReadSync(result.get())) {
      local_tasks_->Push(std::move(*task));
      continue;
    }

    if (result->HasExtension(proto::Status::extension)) {
      const auto& status = result->GetExtension(proto::Status::extension);
      if (status.code() != proto::Status::OK) {
        LOG(WARNING) << "Remote compilation failed with error(s):" << std::endl
                     << status.description();
        failed_tasks_->Push(std::move(*task));
        continue;
      }
    }

    if (result->HasExtension(proto::RemoteResult::extension)) {
      const auto& extension =
          result->GetExtension(proto::RemoteResult::extension);
      String output_path =
          message->current_dir() + "/" + message->flags().output();
      if (base::WriteFile(output_path, extension.obj())) {
        proto::Status status;
        status.set_code(proto::Status::OK);
        LOG(INFO) << "Remote compilation successful: "
                  << message->flags().input();
        UpdateCacheFromRemote(message, extension, status);
        task->first->ReportStatus(status);
        continue;
      }
    }

    // In case this task has crashed the remote end, we will try only local
    // compilation next time.
    failed_tasks_->Push(std::move(*task));
  }
}

void Daemon::DoLocalExecution(const std::atomic<bool>& is_shutting_down) {
  while (!is_shutting_down) {
    Optional&& task = all_tasks_->Pop();
    if (!task) {
      break;
    }

    if (!task->second->remote()) {
      proto::Status status;
      if (!FillFlags(task->second->mutable_flags(), &status)) {
        task->first->ReportStatus(status);
        continue;
      }

      String error;
      ui32 uid = task->second->has_user_id() ? task->second->user_id()
                                             : base::Process::SAME_UID;
      base::ProcessPtr process = CreateProcess(task->second->flags(), uid,
                                               task->second->current_dir());
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
                  << task->second->flags().input();
        UpdateCacheFromFlags(task->second.get(), status);
      }

      task->first->ReportStatus(status);
    } else {
      // Check that we have a compiler of a requested version.
      proto::Status status;
      if (!FillFlags(task->second->mutable_flags(), &status)) {
        task->first->ReportStatus(status);
        continue;
      }

      task->second->mutable_flags()->set_output("-");
      task->second->mutable_flags()->clear_input();
      task->second->mutable_flags()->clear_deps_file();

      // Optimize compilation for preprocessed code for some languages.
      if (task->second->flags().has_language()) {
        if (task->second->flags().language() == "c") {
          task->second->mutable_flags()->set_language("cpp-output");
        } else if (task->second->flags().language() == "c++") {
          task->second->mutable_flags()->set_language("c++-cpp-output");
        } else if (task->second->flags().language() == "objective-c++") {
          task->second->mutable_flags()->set_language(
              "objective-c++-cpp-output");
        }
      }

      // Do local compilation. Pipe the input file to the compiler and read
      // output file from the compiler's stdout.
      String error;
      base::ProcessPtr process = CreateProcess(task->second->flags());
      if (!process->Run(10, task->second->pp_source(), &error)) {
        std::stringstream arguments;
        arguments << "Arguments:";
        for (const auto& flag : task->second->flags().other()) {
          arguments << " " << flag;
        }
        arguments << std::endl;
        arguments << "Input size: " << task->second->pp_source().size()
                  << std::endl;
        arguments << "Language: " << task->second->flags().language()
                  << std::endl;
        arguments << std::endl;

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
        LOG(VERBOSE) << arguments.str();
      } else {
        status.set_code(proto::Status::OK);
        status.set_description(process->stderr());
        LOG(INFO) << "External compilation successful";
      }

      Daemon::ScopedMessage message(new proto::Universal);
      const auto& result = proto::RemoteResult::extension;
      message->MutableExtension(proto::Status::extension)->CopyFrom(status);
      message->MutableExtension(result)->set_obj(process->stdout());

      if (cache_config_ && cache_config_->remote() &&
          status.code() == proto::Status::OK) {
        UpdateCacheFromRemote(task->second.get(), message->GetExtension(result),
                              status);
      }

      task->first->SendAsync(std::move(message));
    }
  }
}

}  // namespace daemon
}  // namespace dist_clang
