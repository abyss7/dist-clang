#include <daemon/base_daemon.h>

#include <base/file_utils.h>
#include <base/logging.h>
#include <base/process_impl.h>
#include <net/connection.h>
#include <net/network_service_impl.h>

#include <base/using_log.h>

namespace dist_clang {

using namespace file_cache::string;

namespace {

inline CommandLine CommandLineForCache(const proto::Flags& flags) {
  String command_line =
      base::JoinString<' '>(flags.other().begin(), flags.other().end());
  if (flags.has_language()) {
    command_line += " -x " + flags.language();
  }
  if (flags.cc_only_size()) {
    command_line += " " + base::JoinString<' '>(flags.cc_only().begin(),
                                                flags.cc_only().end());
  }

  return CommandLine(command_line);
}

inline CommandLine CommandLineForDirect(const proto::Flags& flags) {
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

  return CommandLine(command_line);
}

bool ParseDeps(const String& deps, const String& base_path,
               List<String>& headers) {
  List<String> lines;
  base::SplitString<'\n'>(deps, lines);
  if (lines.empty()) {
    return false;
  }

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

bool BaseDaemon::Initialize() {
  if (conf_.has_cache() && !conf_.cache().disabled()) {
    cache_.reset(new FileCache(conf_.cache().path(), conf_.cache().size(),
                               conf_.cache().snappy()));
    if (!cache_->Run()) {
      cache_.reset();
    }
  }

  for (const auto& version : conf_.versions()) {
    if (!version.has_path() || version.path().empty()) {
      LOG(ERROR) << "Compiler " << version.version() << " has no path.";
      return false;
    }
    compilers_.emplace(version.version(), version.path());

    // Load plugins.
    auto& plugin_map =
        plugins_.emplace(version.version(), PluginNameMap()).first->second;
    for (const auto& plugin : version.plugins()) {
      if (!plugin.has_path() || plugin.path().empty()) {
        LOG(ERROR) << "Plugin " << plugin.name() << " for compiler "
                   << version.version() << " has no path.";
        return false;
      }
      plugin_map.emplace(plugin.name(), plugin.path());
    }
  }

  return network_service_->Run();
}

BaseDaemon::BaseDaemon(const proto::Configuration& configuration)
    : conf_(configuration),
      resolver_(net::EndPointResolver::Create()),
      network_service_(net::NetworkService::Create()) {
  conf_.CheckInitialized();

  // Setup log's verbosity early - even before configuration integrity check.
  // Everything else is done in the method |Initialize()|.
  if (conf_.has_verbosity()) {
    base::Log::RangeSet ranges;
    for (const auto& level : conf_.verbosity().levels()) {
      if (level.has_left() && level.left() > level.right()) {
        continue;
      }

      if (!level.has_left()) {
        ranges.emplace(level.right(), level.right());
      } else {
        ranges.emplace(level.left(), level.right());
      }
    }

    base::Log::RangeSet range_set;
    if (!ranges.empty()) {
      auto current = *ranges.begin();
      if (ranges.size() > 1) {
        for (auto it = std::next(ranges.begin()); it != ranges.end(); ++it) {
          if (current.second + 1 >= it->first) {
            current.second = std::max(it->second, current.second);
          } else {
            range_set.emplace(current.second, current.first);
            current = *it;
          }
        }
      }
      range_set.emplace(current.second, current.first);
    }
    base::Log::Reset(conf_.verbosity().error_mark(), std::move(range_set));
  }
}

BaseDaemon::~BaseDaemon() {
  network_service_.reset();
}

HandledHash BaseDaemon::GenerateHash(const proto::Flags& flags,
                                     const HandledSource& code) const {
  const Version version(flags.compiler().version());
  return FileCache::Hash(code, CommandLineForCache(flags), version);
}

bool BaseDaemon::SetupCompiler(proto::Flags* flags,
                               proto::Status* status) const {
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

bool BaseDaemon::SearchSimpleCache(const proto::Flags& flags,
                                   const HandledSource& source,
                                   FileCache::Entry* entry) const {
  DCHECK(entry);

  if (!cache_) {
    return false;
  }

  const Version version(flags.compiler().version());
  const auto command_line = CommandLineForCache(flags);

  if (!cache_->Find(source, command_line, version, entry)) {
    LOG(CACHE_INFO) << "Cache miss: " << flags.input();
    return false;
  }

  return true;
}

bool BaseDaemon::SearchDirectCache(const proto::Flags& flags,
                                   const String& current_dir,
                                   FileCache::Entry* entry) const {
  DCHECK(conf_.has_emitter() && !conf_.has_absorber());
  DCHECK(flags.has_input() && flags.input()[0] != '/');

  if (!cache_ || !conf_.cache().direct()) {
    return false;
  }

  const Version version(flags.compiler().version());
  const String input = current_dir + "/" + flags.input();
  const CommandLine command_line(CommandLineForDirect(flags));

  UnhandledSource code;
  if (!base::ReadFile(input, &code.str)) {
    return false;
  }

  if (!cache_->Find(code, command_line, version, entry)) {
    LOG(CACHE_INFO) << "Direct cache miss: " << flags.input();
    return false;
  }

  return true;
}

void BaseDaemon::UpdateSimpleCache(const proto::Flags& flags,
                                   const HandledSource& source,
                                   const FileCache::Entry& entry) {
  const Version version(flags.compiler().version());
  const auto command_line = CommandLineForCache(flags);

  if (!cache_) {
    return;
  }

  if (conf_.cache().sync()) {
    cache_->StoreNow(source, command_line, version, entry);
  } else {
    cache_->Store(source, command_line, version, entry);
  }
}

void BaseDaemon::UpdateDirectCache(const proto::LocalExecute* message,
                                   const HandledSource& source,
                                   const FileCache::Entry& entry) {
  const auto& flags = message->flags();

  DCHECK(conf_.has_emitter() && !conf_.has_absorber());
  DCHECK(flags.has_input() && flags.input()[0] != '/');

  if (!cache_ || !conf_.cache().direct()) {
    return;
  }

  if (entry.deps.empty()) {
    LOG(CACHE_WARNING) << "Can't update direct cache without deps : "
                       << flags.input();
    return;
  }

  const Version version(flags.compiler().version());
  const auto hash = cache_->Hash(source, CommandLineForCache(flags), version);
  const auto command_line = CommandLineForDirect(flags);
  const String input_path = message->current_dir() + "/" + flags.input();
  List<String> headers;
  UnhandledSource original_code;

  if (ParseDeps(entry.deps, message->current_dir(), headers) &&
      base::ReadFile(input_path, &original_code.str)) {
    cache_->Store(original_code, command_line, version, headers, hash);
  } else {
    LOG(CACHE_ERROR) << "Failed to parse deps or read input " << input_path;
  }
}

// static
base::ProcessPtr BaseDaemon::CreateProcess(const proto::Flags& flags,
                                           ui32 user_id, Immutable cwd_path) {
  DCHECK(flags.compiler().has_path());
  base::ProcessPtr process =
      base::Process::Create(flags.compiler().path(), cwd_path, user_id);

  // |flags.other()| always must go first, since it contains the "-cc1" flag.
  process->AppendArg(flags.other().begin(), flags.other().end());
  process->AppendArg(Immutable(flags.action()));
  process->AppendArg(flags.non_cached().begin(), flags.non_cached().end());
  process->AppendArg(flags.non_direct().begin(), flags.non_direct().end());
  for (const auto& plugin : flags.compiler().plugins()) {
    process->AppendArg("-load"_l).AppendArg(Immutable(plugin.path()));
  }
  if (flags.has_deps_file()) {
    process->AppendArg("-dependency-file"_l)
        .AppendArg(Immutable(flags.deps_file()));
  }
  if (flags.has_language()) {
    process->AppendArg("-x"_l).AppendArg(Immutable(flags.language()));
  }
  if (flags.has_output()) {
    process->AppendArg("-o"_l).AppendArg(Immutable(flags.output()));
  }
  if (flags.has_input()) {
    process->AppendArg(Immutable(flags.input()));
  }

  return std::move(process);
}

// static
base::ProcessPtr BaseDaemon::CreateProcess(const proto::Flags& flags,
                                           Immutable cwd_path) {
  return std::move(CreateProcess(flags, base::Process::SAME_UID, cwd_path));
}

void BaseDaemon::HandleNewConnection(net::ConnectionPtr connection) {
  using namespace std::placeholders;

  auto callback = std::bind(&BaseDaemon::HandleNewMessage, this, _1, _2, _3);
  connection->ReadAsync(callback);
}

}  // namespace daemon
}  // namespace dist_clang
