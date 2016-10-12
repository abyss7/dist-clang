#include <daemon/compilation_daemon.h>

#include <base/assert.h>
#include <base/file/file.h>
#include <base/logging.h>
#include <base/process_impl.h>
#include <base/string_utils.h>

#include <base/using_log.h>

namespace dist_clang {

using cache::ExtraFiles;
using namespace cache::string;

namespace {

// Returns relative path from |current_dir| to |input|. If |input| does
// not contain |current_dir| as a prefix or is smaller, returns |input|
// untouched. If |input| is equal to |current_dir|, returns ".".
// |current_dir| must not end with '/'.
inline String GetRelativePath(const String& current_dir, const String& input) {
  if (input.size() < current_dir.size()) {
    return input;
  } else if (input.size() == current_dir.size()) {
    return input == current_dir ? String(".") : input;
  }
  return input.substr(0, current_dir.size()) == current_dir
             ? input.substr(current_dir.size() + 1)
             : input;
}

inline CommandLine CommandLineForSimpleCache(const base::proto::Flags& flags) {
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

inline CommandLine CommandLineForDirectCache(const String& current_dir,
                                             const base::proto::Flags& flags) {
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

  // Compiler implicitly appends file's directory as an include path - so do we.
  String input_path = flags.input().substr(0, flags.input().find_last_of('/'));
  command_line += " -I" + GetRelativePath(current_dir, input_path);

  return CommandLine(command_line);
}

bool ParseDeps(String deps, List<String>& headers) {
  base::Replace(deps, "\\\n", "");

  List<String> lines;
  base::SplitString<'\n'>(deps, lines);

  auto deps_start = lines.front().find(':');
  if (deps_start == String::npos) {
    return false;
  }
  deps_start++;

  base::SplitString<' '>(lines.front().substr(deps_start), headers);

  return true;
}

inline String GetFullPath(const String& current_dir, const String& path) {
  return path[0] == '/' ? path : current_dir + "/" + path;
}

}  // namespace

namespace daemon {

bool CompilationDaemon::Initialize() {
  if (conf_->has_cache() && !conf_->cache().disabled()) {
    cache_.reset(new cache::FileCache(
        conf_->cache().path(), conf_->cache().size(), conf_->cache().snappy(),
        conf_->cache().store_index()));
    if (!cache_->Run(conf_->cache().clean_period())) {
      cache_.reset();
    }
  }
  if (!UpdateConfiguration(*conf_)) {
    return false;
  }

  return BaseDaemon::Initialize();
}

bool CompilationDaemon::UpdateConfiguration(
    const proto::Configuration& configuration) {
  CHECK(conf_->IsInitialized());
  for (const auto& version : configuration.versions()) {
    if (!version.has_path() || version.path().empty()) {
      LOG(ERROR) << "Compiler " << version.version() << " has no path.";
      return false;
    }
    for (const auto& plugin : version.plugins()) {
      if (!plugin.has_path() || plugin.path().empty()) {
        LOG(ERROR) << "Plugin " << plugin.name() << " for compiler "
                   << version.version() << " has no path.";
        return false;
      }
    }
  }
  conf_.reset(new proto::Configuration(configuration));
  return BaseDaemon::UpdateConfiguration(configuration);
}

CompilationDaemon::CompilationDaemon(const proto::Configuration& configuration)
    : BaseDaemon(configuration) {
  conf_.reset(new proto::Configuration(configuration));
  conf_->CheckInitialized();

  // Setup log's verbosity early - even before configuration integrity check.
  // Everything else is done in the method |Initialize()|.
  if (conf_->has_verbosity()) {
    base::Log::RangeSet ranges;
    for (const auto& level : conf_->verbosity().levels()) {
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
    base::Log::Reset(conf_->verbosity().error_mark(), std::move(range_set));
  }
}

HandledHash CompilationDaemon::GenerateHash(
    const base::proto::Flags& flags, const HandledSource& code,
    const ExtraFiles& extra_files) const {
  const Version version(flags.compiler().version());
  return cache::FileCache::Hash(code, extra_files,
                                CommandLineForSimpleCache(flags), version);
}

bool CompilationDaemon::SetupCompiler(base::proto::Flags* flags,
                                      net::proto::Status* status) const {
  auto config = conf();
  auto getCompilerPath = [&config](String version, String* path) {
    for (const auto& conf_version : config->versions()) {
      if (conf_version.version() == version) {
        path->assign(conf_version.path());
        return true;
      }
    }
    return false;
  };

  // No flags - filled flags.
  if (!flags) {
    return true;
  }

  if (!flags->compiler().has_path()) {
    if (!getCompilerPath(flags->compiler().version(),
                         flags->mutable_compiler()->mutable_path())) {
      if (status) {
        status->set_code(net::proto::Status::NO_VERSION);
        status->set_description("Compiler not found: " +
                                flags->compiler().version());
      }
      return false;
    }
  }

  auto getPluginsByCompilerVersion = [&config](String version,
                                               PluginNameMap& plugin_map) {
    for (const auto& conf_version : config->versions()) {
      if (conf_version.version() == version) {
        const auto& conf_plugins = conf_version.plugins();
        for (const auto& conf_plugin : conf_plugins) {
          plugin_map.emplace(conf_plugin.name(), conf_plugin.path());
        }
      }
    }
  };

  PluginNameMap plugin_map;
  getPluginsByCompilerVersion(flags->compiler().version(), plugin_map);
  for (auto& flag_plugin : *flags->mutable_compiler()->mutable_plugins()) {
    if (!flag_plugin.has_path()) {
      auto plugin_by_name = plugin_map.find(flag_plugin.name());
      if (plugin_by_name == plugin_map.end()) {
        if (status) {
          status->set_code(net::proto::Status::NO_VERSION);
          status->set_description("Plugin not found: " +
                                  flags->compiler().version());
        }
        return false;
      }
      flag_plugin.set_path(plugin_by_name->second);
    }
  }

  return true;
}

bool CompilationDaemon::ReadExtraFiles(const base::proto::Flags& flags,
                                       const String& current_dir,
                                       ExtraFiles* extra_files) const {
  DCHECK(extra_files);
  DCHECK(extra_files->empty());

  if (!flags.has_sanitize_blacklist()) {
    return true;
  }

  Immutable sanitize_blacklist_contents;
  const String sanitize_blacklist =
      GetFullPath(current_dir, flags.sanitize_blacklist());
  if (!base::File::Read(sanitize_blacklist, &sanitize_blacklist_contents)) {
    LOG(CACHE_ERROR) << "Failed to read sanitize blacklist file"
                     << sanitize_blacklist;
    return false;
  }
  extra_files->emplace(cache::SANITIZE_BLACKLIST,
                       std::move(sanitize_blacklist_contents));
  return true;
}

bool CompilationDaemon::SearchSimpleCache(
    const base::proto::Flags& flags, const HandledSource& source,
    const ExtraFiles& extra_files, cache::FileCache::Entry* entry) const {
  if (!cache_) {
    return false;
  }

  const Version version(flags.compiler().version());
  const auto command_line = CommandLineForSimpleCache(flags);

  if (!cache_->Find(source, extra_files, command_line, version, entry)) {
    LOG(CACHE_INFO) << "Cache miss: " << flags.input();
    return false;
  }

  return true;
}

bool CompilationDaemon::SearchDirectCache(
    const base::proto::Flags& flags, const String& current_dir,
    cache::FileCache::Entry* entry) const {
  auto config = conf();
  DCHECK(config->has_emitter() && !config->has_absorber());
  DCHECK(flags.has_input());

  if (!cache_ || !config->cache().direct()) {
    return false;
  }

  const Version version(flags.compiler().version());
  const String input = GetFullPath(current_dir, flags.input());
  const CommandLine command_line(CommandLineForDirectCache(current_dir, flags));

  UnhandledSource code;
  if (!base::File::Read(input, &code.str)) {
    return false;
  }

  ExtraFiles extra_files;
  if (!ReadExtraFiles(flags, current_dir, &extra_files)) {
    return false;
  }

  if (!cache_->Find(code, extra_files, command_line, version, current_dir,
                    entry)) {
    LOG(CACHE_INFO) << "Direct cache miss: " << flags.input();
    return false;
  }

  return true;
}

void CompilationDaemon::UpdateSimpleCache(
    const base::proto::Flags& flags, const HandledSource& source,
    const ExtraFiles& extra_files, const cache::FileCache::Entry& entry) {
  const Version version(flags.compiler().version());
  const auto command_line = CommandLineForSimpleCache(flags);

  if (!cache_) {
    return;
  }

  cache_->Store(source, extra_files, command_line, version, entry);
}

void CompilationDaemon::UpdateDirectCache(
    const base::proto::Local* message, const HandledSource& source,
    const ExtraFiles& extra_files, const cache::FileCache::Entry& entry) {
  const auto& flags = message->flags();
  auto config = conf();
  DCHECK(config->has_emitter() && !config->has_absorber());
  DCHECK(flags.has_input());

  if (!cache_ || !config->cache().direct()) {
    return;
  }

  if (entry.deps.empty()) {
    LOG(CACHE_WARNING) << "Can't update direct cache without deps : "
                       << flags.input();
    return;
  }

  const Version version(flags.compiler().version());
  const auto hash = cache_->Hash(source, extra_files,
                                 CommandLineForSimpleCache(flags), version);
  const auto command_line =
      CommandLineForDirectCache(message->current_dir(), flags);
  const String input_path = GetFullPath(message->current_dir(), flags.input());
  List<String> headers;
  UnhandledSource original_code;

  if (ParseDeps(entry.deps, headers) &&
      base::File::Read(input_path, &original_code.str)) {
    cache_->Store(original_code, extra_files, command_line, version, headers,
                  message->current_dir(), hash);
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

  // FIXME: looks like the --internal-isystem flags order is important.
  //        We should preserve an original order.
  process->AppendArg(flags.non_direct().begin(), flags.non_direct().end());
  process->AppendArg(flags.non_cached().begin(), flags.non_cached().end());

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
  if (flags.has_sanitize_blacklist()) {
    process->AppendArg(Immutable("-fsanitize-blacklist="_l) +
                       Immutable(flags.sanitize_blacklist()));
  }
  if (flags.has_output()) {
    process->AppendArg("-o"_l).AppendArg(Immutable(flags.output()));
  }
  if (flags.has_input()) {
    process->AppendArg(Immutable(flags.input()));
  }

  return process;
}

// static
base::ProcessPtr CompilationDaemon::CreateProcess(
    const base::proto::Flags& flags, Immutable cwd_path) {
  return CreateProcess(flags, base::Process::SAME_UID, cwd_path);
}

}  // namespace daemon
}  // namespace dist_clang
