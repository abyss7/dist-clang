#include <daemon/compilation_daemon.h>

#include <base/file/file.h>
#include <base/logging.h>
#include <base/process_impl.h>
#include <base/string_utils.h>

#include <base/using_log.h>

namespace dist_clang {

using namespace cache::string;

namespace {

inline CommandLine CommandLineForCache(const base::proto::Flags& flags) {
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

inline CommandLine CommandLineForDirect(const base::proto::Flags& flags,
                                        const String& base_path) {
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

  // TODO: write test on this case.
  // Compiler implicitly appends file directory as an include path - so do we.
  String input_path = flags.input().substr(0, flags.input().find_last_of('/'));
  if (input_path[0] != '/') {
    input_path = base_path + '/' + input_path;
  }
  command_line += " -I" + input_path;

  return CommandLine(command_line);
}

bool ParseDeps(String deps, const String& base_path, List<String>& headers) {
  base::Replace(deps, "\\\n", "");

  List<String> lines;
  base::SplitString<'\n'>(deps, lines);

  auto deps_start = lines.front().find(':');
  if (deps_start == String::npos) {
    return false;
  }
  deps_start++;

  base::SplitString<' '>(lines.front().substr(deps_start), headers);

  for (auto& header : headers) {
    if (header[0] != '/') {
      header = base_path + "/" + header;
    }
  }

  return true;
}

}  // namespace

namespace daemon {

bool CompilationDaemon::Initialize() {
  if (conf_.has_cache() && !conf_.cache().disabled()) {
    cache_.reset(new cache::FileCache(
        conf_.cache().path(), conf_.cache().size(), conf_.cache().snappy()));
    if (!cache_->Run(conf_.cache().clean_period())) {
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

  return BaseDaemon::Initialize();
}

CompilationDaemon::CompilationDaemon(const proto::Configuration& configuration)
    : BaseDaemon(configuration), conf_(configuration) {
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

HandledHash CompilationDaemon::GenerateHash(const base::proto::Flags& flags,
                                            const HandledSource& code) const {
  const Version version(flags.compiler().version());
  return cache::FileCache::Hash(code, CommandLineForCache(flags), version);
}

bool CompilationDaemon::SetupCompiler(base::proto::Flags* flags,
                                      net::proto::Status* status) const {
  // No flags - filled flags.
  if (!flags) {
    return true;
  }

  if (!flags->compiler().has_path()) {
    auto compiler = compilers_.find(flags->compiler().version());
    if (compiler == compilers_.end()) {
      if (status) {
        status->set_code(net::proto::Status::NO_VERSION);
        status->set_description("Compiler not found: " +
                                flags->compiler().version());
      }
      return false;
    }
    flags->mutable_compiler()->set_path(compiler->second);
  }

  auto plugin_map = plugins_.find(flags->compiler().version());
  auto& plugins = *flags->mutable_compiler()->mutable_plugins();
  for (auto& plugin : plugins) {
    if (!plugin.has_path()) {
      if (plugin_map == plugins_.end()) {
        if (status) {
          status->set_code(net::proto::Status::NO_VERSION);
          status->set_description("Plugin " + plugin.name() + " not found: " +
                                  flags->compiler().version());
        }
        return false;
      }
      auto plugin_by_name = plugin_map->second.find(plugin.name());
      if (plugin_by_name == plugin_map->second.end()) {
        if (status) {
          status->set_code(net::proto::Status::NO_VERSION);
          status->set_description("Plugin " + plugin.name() + " not found: " +
                                  flags->compiler().version());
        }
        return false;
      }
      plugin.set_path(plugin_by_name->second);
    }
  }

  return true;
}

bool CompilationDaemon::SearchSimpleCache(
    const base::proto::Flags& flags, const HandledSource& source,
    cache::FileCache::Entry* entry) const {
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

bool CompilationDaemon::SearchDirectCache(
    const base::proto::Flags& flags, const String& current_dir,
    cache::FileCache::Entry* entry) const {
  DCHECK(conf_.has_emitter() && !conf_.has_absorber());
  DCHECK(flags.has_input());

  if (!cache_ || !conf_.cache().direct()) {
    return false;
  }

  const Version version(flags.compiler().version());
  const String input = flags.input()[0] != '/'
                           ? current_dir + "/" + flags.input()
                           : flags.input();
  const CommandLine command_line(CommandLineForDirect(flags, current_dir));

  UnhandledSource code;
  if (!base::File::Read(input, &code.str)) {
    return false;
  }

  if (!cache_->Find(code, command_line, version, entry)) {
    LOG(CACHE_INFO) << "Direct cache miss: " << flags.input();
    return false;
  }

  return true;
}

void CompilationDaemon::UpdateSimpleCache(
    const base::proto::Flags& flags, const HandledSource& source,
    const cache::FileCache::Entry& entry) {
  const Version version(flags.compiler().version());
  const auto command_line = CommandLineForCache(flags);

  if (!cache_) {
    return;
  }

  cache_->Store(source, command_line, version, entry);
}

void CompilationDaemon::UpdateDirectCache(
    const base::proto::Local* message, const HandledSource& source,
    const cache::FileCache::Entry& entry) {
  const auto& flags = message->flags();

  DCHECK(conf_.has_emitter() && !conf_.has_absorber());
  DCHECK(flags.has_input());

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
  const auto command_line = CommandLineForDirect(flags, message->current_dir());
  const String input_path = flags.input()[0] != '/'
                                ? message->current_dir() + "/" + flags.input()
                                : flags.input();
  List<String> headers;
  UnhandledSource original_code;

  if (ParseDeps(entry.deps, message->current_dir(), headers) &&
      base::File::Read(input_path, &original_code.str)) {
    cache_->Store(original_code, command_line, version, headers, hash);
  } else {
    LOG(CACHE_ERROR) << "Failed to parse deps or read input " << input_path;
  }
}

// static
base::ProcessPtr CompilationDaemon::CreateProcess(
    const base::proto::Flags& flags, ui32 user_id, Immutable cwd_path) {
  DCHECK(flags.compiler().has_path());
  base::ProcessPtr process =
      base::Process::Create(flags.compiler().path(), cwd_path, user_id);

  // |flags.other()| always must go first, since it contains the "-cc1" flag.
  process->AppendArg(flags.other().begin(), flags.other().end());
  process->AppendArg(Immutable(flags.action()));
  process->AppendArg(flags.non_cached().begin(), flags.non_cached().end());
  process->AppendArg(flags.non_direct().begin(), flags.non_direct().end());

  // TODO: render args using libclang
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
base::ProcessPtr CompilationDaemon::CreateProcess(
    const base::proto::Flags& flags, Immutable cwd_path) {
  return std::move(CreateProcess(flags, base::Process::SAME_UID, cwd_path));
}

}  // namespace daemon
}  // namespace dist_clang
