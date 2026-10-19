#include <daemon/compilation_daemon.h>

#include <base/assert.h>
#include <base/constants.h>
#include <base/file/file.h>
#include <base/logging.h>
#include <base/platform.h>
#include <base/process_impl.h>
#include <base/string_utils.h>
#include <daemon/driver_args.h>
#include <net/connection.h>
#include <perf/stat_reporter.h>

#include STL(random)

#include <sys/stat.h>

#include <base/using_log.h>

namespace dist_clang {

using cache::ExtraFiles;
using perf::proto::Metric;
using namespace cache::string;

using Counter = perf::Counter<perf::StatReporter>;

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

template <class... Args>
inline auto CreateArgumentList(const Args&... args) {
  MultiMap<ui32, String> map;
  List<String> list;

  for (const auto& arg_list : {args...}) {
    for (const auto& arg : arg_list) {
      for (const auto& value : arg.values()) {
        map.emplace(arg.index(), value);
      }
    }
  }

  for (const auto& arg : map) {
    list.push_back(arg.second);
  }

  return list;
}

const char kRootPlaceholder[] = "<ROOT>";

// Checks, that the path at |pos| starts a token, maybe after a flag name:
// "path", "-Ipath", "-isystem path", "=path", "\"path\"".
bool IsPathStart(const String& text, size_t pos) {
  size_t token_start = pos;
  while (token_start > 0 && !strchr(" \"=,\n", text[token_start - 1])) {
    --token_start;
  }
  if (token_start == pos) {
    return true;
  }
  if (text[token_start] != '-') {
    return false;
  }
  for (size_t i = token_start; i < pos; ++i) {
    if (!isalpha(text[i]) && text[i] != '-' && text[i] != '+') {
      return false;
    }
  }
  return true;
}

// Replaces |prefix| with the placeholder, where it's a whole prefix of a path.
void ReplacePathPrefix(String& text, const String& prefix,
                       const char* placeholder = kRootPlaceholder) {
  if (prefix.empty() || prefix == ".") {
    return;
  }
  size_t pos = 0;
  while ((pos = text.find(prefix, pos)) != String::npos) {
    const size_t end = pos + prefix.size();
    if ((end == text.size() || text[end] == '/' || text[end] == '"' ||
         text[end] == ' ' || text[end] == '\n') &&
        IsPathStart(text, pos)) {
      text.replace(pos, prefix.size(), placeholder);
      pos += strlen(placeholder);
    } else {
      pos = end;
    }
  }
}

// The arguments with the paths, that the compiler remaps in the result: the
// include directories and files, the maps and the compilation directories.
// The other paths - e.g. in the definitions of the macros - get into the result
// as is.
bool HasRemappedPaths(const String& name) {
  static const char* const kPrefixes[] = {
      "-I",
      "-isystem",
      "-iquote",
      "-idirafter",
      "-cxx-isystem",
      "-stdlib++-isystem",
      "-include",
      "-imacros",
      "-F",
      "-iframework",
      "-fmacro-prefix-map=",
      "-ffile-prefix-map=",
      "-fdebug-prefix-map=",
      "-fcoverage-prefix-map=",
      "-fdebug-compilation-dir",
      "-ffile-compilation-dir=",
      "-fcoverage-compilation-dir=",
  };
  for (const char* prefix : kPrefixes) {
    if (name.starts_with(prefix)) {
      return true;
    }
  }
  return false;
}

// The values of the arguments in the key - with the root replaced in the paths,
// that the compiler remaps.
template <class... Args>
inline auto CreateKeyArgumentList(const base::proto::Flags& flags,
                                  const Args&... args) {
  MultiMap<ui32, String> map;
  List<String> list;

  for (const auto& arg_list : {args...}) {
    for (const auto& arg : arg_list) {
      const bool remapped =
          arg.values_size() && HasRemappedPaths(arg.values(0));
      for (const auto& value : arg.values()) {
        map.emplace(arg.index(),
                    remapped ? daemon::CompilationDaemon::NormalizePaths(
                                   value, flags)
                             : value);
      }
    }
  }

  for (const auto& arg : map) {
    list.push_back(arg.second);
  }

  return list;
}

// Parts of the cache key that aren't flags.
inline String KeyExtra(const base::proto::Flags& flags) {
  String result;
  for (const auto& part : flags.key_extra()) {
    result += "\n" + part;
  }
  return result;
}

inline CommandLine CommandLineForSimpleCache(const base::proto::Flags& flags) {
  auto arg_list =
      flags.recheck_warnings()
          ? CreateKeyArgumentList(flags, flags.other(), flags.cc_only())
          : CreateKeyArgumentList(flags, flags.other(), flags.cc_only(),
                                  flags.warnings());
  auto command_line = base::JoinString<' '>(arg_list.begin(), arg_list.end());
  if (flags.has_language()) {
    command_line += " -x " + flags.language();
  }
  command_line += KeyExtra(flags);

  return CommandLine(command_line);
}

inline CommandLine CommandLineForDirectCache(const String& current_dir,
                                             const base::proto::Flags& flags) {
  auto args_list =
      flags.recheck_warnings()
          ? CreateKeyArgumentList(flags, flags.other(), flags.cc_only(),
                                  flags.non_cached())
          : CreateKeyArgumentList(flags, flags.other(), flags.cc_only(),
                                  flags.non_cached(), flags.warnings());
  auto command_line = base::JoinString<' '>(args_list.begin(), args_list.end());
  if (flags.has_language()) {
    command_line += " -x " + flags.language();
  }

  // Compiler implicitly appends file's directory as an include path - so do we.
  // FIXME: make sure we did it in a proper order - relative to other include
  //        paths.
  using daemon::CompilationDaemon;
  String input_path = flags.input().substr(0, flags.input().find_last_of('/'));
  command_line += " " + CompilationDaemon::NormalizePaths(
                            "-I" + GetRelativePath(current_dir, input_path),
                            flags);

  // The name of the source gets into the result - through "__FILE__" and the
  // debug info: the sources with the same contents aren't interchangeable.
  command_line +=
      " " + CompilationDaemon::NormalizePaths(
                GetRelativePath(current_dir, flags.input()), flags);
  command_line += KeyExtra(flags);

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
  auto conf = this->conf();

  if (conf->has_cache() && !conf->cache().disabled()) {
    // Snappy is only for the old caches.
    auto compression =
        conf->cache().compression() == proto::Configuration::Cache::NONE
            ? cache::proto::NONE
            : cache::proto::ZSTD;
    if (!conf->cache().has_compression() && conf->cache().has_snappy() &&
        !conf->cache().snappy()) {
      compression = cache::proto::NONE;
    }
    cache_ = std::make_unique<cache::FileCache>(
        conf->cache().path(), conf->cache().size(), compression,
        conf->cache().store_index(),
        conf->cache().link_outputs() && conf->has_emitter());
    cache_->SetMinFreePercent(conf->cache().min_free_percent());
    cache_->SetCompressAfter(conf->cache().compress_after());
    cache_->SetGhostsTtl(conf->cache().ghosts_ttl());
    cache_->SetStoreTimeTraces(conf->cache().store_time_traces());
    if (!cache_->Run(conf->cache().clean_period())) {
      cache_.reset();
    }
  }

  return BaseDaemon::Initialize();
}

void CompilationDaemon::Flush() {
  if (cache_) {
    cache_->Close();
  }
}

CompilationDaemon::CompilationDaemon(const Configuration& conf)
    : BaseDaemon(conf) {
  // Setup log verbosity early - even before configuration integrity check.
  // Everything else is done in the method |Initialize()|.
  if (conf.has_verbosity()) {
    base::Log::RangeSet ranges;
    for (const auto& level : conf.verbosity().levels()) {
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
    base::Log::Reset(conf.verbosity().error_mark(), std::move(range_set));
  }
}

namespace {

const char kDepsTargetPlaceholder[] = "@DIST_CLANG_TARGET@";
const char kDepsRootPlaceholder[] = "@DIST_CLANG_ROOT@";
const char kDepsRelativeRootPlaceholder[] = "@DIST_CLANG_RELATIVE_ROOT@";

}  // namespace

// static
Immutable CompilationDaemon::NormalizeDeps(Immutable deps,
                                           const base::proto::Flags& flags) {
  String content = deps.string_copy();
  // The targets end with the first colon, that isn't escaped: the compiler
  // may wrap their line.
  if (!flags.deps_target().empty()) {
    size_t colon = 0;
    while ((colon = content.find(':', colon)) != String::npos && colon > 0 &&
           content[colon - 1] == '\\') {
      ++colon;
    }
    if (colon != String::npos) {
      content = kDepsTargetPlaceholder + content.substr(colon);
    }
  }
  if (flags.has_key_root()) {
    ReplacePathPrefix(content, flags.key_root(), kDepsRootPlaceholder);
    ReplacePathPrefix(content, flags.key_root_relative(),
                      kDepsRelativeRootPlaceholder);
  }
  return Immutable(std::move(content));
}

// static
Immutable CompilationDaemon::RestoreDeps(Immutable deps,
                                         const base::proto::Flags& flags) {
  String content = deps.string_copy();
  if (content.starts_with(kDepsTargetPlaceholder)) {
    content = flags.deps_target() +
              content.substr(strlen(kDepsTargetPlaceholder));
  }
  // The paths of the other root become the ones of the current one.
  base::Replace(content, kDepsRootPlaceholder, flags.key_root().c_str());
  base::Replace(content, kDepsRelativeRootPlaceholder,
                flags.key_root_relative().c_str());
  return Immutable(std::move(content));
}

// static
Immutable CompilationDaemon::NormalizeTimeTrace(
    Immutable trace, const base::proto::Flags& flags) {
  if (!flags.has_key_root()) {
    return trace;
  }
  String content = trace.string_copy();
  ReplacePathPrefix(content, flags.key_root(), kDepsRootPlaceholder);
  ReplacePathPrefix(content, flags.key_root_relative(),
                    kDepsRelativeRootPlaceholder);
  return Immutable(std::move(content));
}

// static
Immutable CompilationDaemon::RestoreTimeTrace(Immutable trace,
                                              const base::proto::Flags& flags) {
  String content = trace.string_copy();
  base::Replace(content, kDepsRootPlaceholder, flags.key_root().c_str());
  base::Replace(content, kDepsRelativeRootPlaceholder,
                flags.key_root_relative().c_str());
  return Immutable(std::move(content));
}

// static
String CompilationDaemon::TimeTraceOptions(const base::proto::Flags& flags) {
  google::protobuf::RepeatedPtrField<base::proto::Argument> options;
  for (const auto& arg : flags.ignored()) {
    if (arg.values_size() && arg.values(0) != "-ftime-trace" &&
        !arg.values(0).starts_with("-ftime-trace=")) {
      *options.Add() = arg;
    }
  }
  auto arg_list = CreateArgumentList(options);
  return base::Hexify(
      Immutable(base::JoinString<' '>(arg_list.begin(), arg_list.end()))
          .Hash());
}

// static
i64 CompilationDaemon::FileSystemTime() {
  return base::FileSystemTime();
}

// static
bool CompilationDaemon::InputsUnchangedSince(const base::proto::Local* message,
                                             Immutable deps, i64 time) {
  const auto& flags = message->flags();
  List<String> inputs;
  if (!deps.empty() && !ParseDeps(deps.string_copy(), inputs)) {
    return false;
  }
  inputs.push_back(flags.input());
  for (const auto& file : flags.key_files()) {
    inputs.push_back(file);
  }

  for (const auto& input : inputs) {
    if (input.empty()) {
      continue;
    }
    struct stat st;
    const String path = GetFullPath(message->current_dir(), input);
    if (stat(path.c_str(), &st) == -1) {
      LOG(CACHE_INFO) << "Input is gone during the compilation: " << path;
      return false;
    }
    const i64 mtime = base::ModificationTime(st);
    const i64 ctime = base::StatusChangeTime(st);
    // The modifications within the same tick of the coarse clock, before the
    // start, have the same time - they are fine.
    if (mtime > time || ctime > time) {
      LOG(CACHE_INFO) << "Input is modified during the compilation: " << path;
      return false;
    }
  }

  return true;
}

// static
String CompilationDaemon::NormalizePaths(const String& text,
                                         const base::proto::Flags& flags) {
  if (!flags.has_key_root()) {
    return text;
  }
  String result = text;
  if (flags.key_root_remapped()) {
    ReplacePathPrefix(result, flags.key_root());
  }
  if (flags.key_root_relative_remapped()) {
    ReplacePathPrefix(result, flags.key_root_relative());
  }
  return result;
}

// static
String CompilationDaemon::NormalizeLineMarkers(const String& source,
                                               const base::proto::Flags& flags) {
  if (!flags.has_key_root()) {
    return source;
  }

  String result;
  result.reserve(source.size());
  size_t line_start = 0;
  while (line_start < source.size()) {
    size_t line_end = source.find('\n', line_start);
    line_end = line_end == String::npos ? source.size() : line_end + 1;

    // Line markers look like: # 123 "path/to/file.h" 1
    if (source.compare(line_start, 2, "# ") == 0 &&
        line_start + 2 < source.size() && isdigit(source[line_start + 2])) {
      result += NormalizePaths(
          source.substr(line_start, line_end - line_start), flags);
    } else {
      result.append(source, line_start, line_end - line_start);
    }
    line_start = line_end;
  }
  return result;
}

// static
String CompilationDaemon::DirectCommandLine(const String& current_dir,
                                            const base::proto::Flags& flags) {
  return CommandLineForDirectCache(current_dir, flags).str.string_copy();
}

// static
bool CompilationDaemon::FindSpecialFile(const base::proto::Flags& flags,
                                        const String& current_dir,
                                        String* path) {
  for (const String* file : {&flags.input(), &flags.output(),
                             &flags.deps_file(), &flags.dwo_file()}) {
    if (file->empty()) {
      continue;
    }
    const String full_path = GetFullPath(current_dir, *file);
    struct stat st;
    if (stat(full_path.c_str(), &st) == 0 && !S_ISREG(st.st_mode)) {
      path->assign(*file);
      return true;
    }
  }
  return false;
}

// static
List<Path> CompilationDaemon::IncludeSearchDirs(
    const base::proto::Flags& flags, const String& current_dir) {
  // The groups of the directories in the order of the search.
  enum Group { QUOTE, ANGLED, SYSTEM, AFTER, GROUPS };
  const std::pair<const char*, Group> kOptions[] = {
      {"-iquote", QUOTE},          {"-I", ANGLED},
      {"-isystem", SYSTEM},        {"-cxx-isystem", SYSTEM},
      {"-stdlib++-isystem", SYSTEM}, {"-idirafter", AFTER},
      {"-isystem-after", AFTER},
  };

  List<String> groups[GROUPS];
  // The quoted includes are searched in the directory of the source first.
  groups[QUOTE].push_back(Path(flags.input()).parent_path().string());

  std::map<ui32, const base::proto::Argument*> args;
  for (const auto& arg : flags.non_cached()) {
    args.emplace(arg.index(), &arg);
  }
  for (const auto& [index, arg] : args) {
    if (!arg->values_size()) {
      continue;
    }
    const String& name = arg->values(0);
    for (const auto& [option, group] : kOptions) {
      if (arg->values_size() == 2 && name == option) {
        groups[group].push_back(arg->values(1));
        break;
      }
      if (arg->values_size() == 1 && name.size() > strlen(option) &&
          name.starts_with(option)) {
        groups[group].push_back(name.substr(strlen(option)));
        break;
      }
    }
  }

  // The compiler searches its own directories after the system ones.
  groups[SYSTEM].insert(groups[SYSTEM].end(),
                        flags.implicit_include_dirs().begin(),
                        flags.implicit_include_dirs().end());

  // The compiler skips the duplicates.
  List<Path> result;
  HashSet<String> seen;
  for (const auto& group : groups) {
    for (const auto& dir : group) {
      Path path = (Path(dir).is_absolute() ? Path(dir) : Path(current_dir) / dir)
                      .lexically_normal();
      String path_str = path.string();
      while (path_str.size() > 1 && path_str.ends_with('/')) {
        path_str.pop_back();
      }
      if (seen.insert(path_str).second) {
        result.emplace_back(path_str);
      }
    }
  }
  return result;
}

// static
String CompilationDaemon::WarningsHash(const base::proto::Flags& flags) {
  // The format of the diagnostics isn't a part of the key either.
  google::protobuf::RepeatedPtrField<base::proto::Argument> format;
  for (const auto& arg : flags.non_direct()) {
    if (arg.values_size() && IsDiagnosticsFormatFlag(arg.values(0))) {
      *format.Add() = arg;
    }
  }
  auto arg_list = CreateArgumentList(flags.warnings(), format);
  return base::Hexify(
      Immutable(base::JoinString<' '>(arg_list.begin(), arg_list.end()))
          .Hash());
}

// static
String CompilationDaemon::StripColors(const String& text) {
  String result;
  result.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    // CSI sequences: ESC '[' parameters final-byte.
    if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
      i += 2;
      while (i < text.size() && !(text[i] >= 0x40 && text[i] <= 0x7e)) {
        ++i;
      }
      continue;
    }
    result.push_back(text[i]);
  }
  return result;
}

// static
HandledHash CompilationDaemon::GenerateUniqueHash(
    const base::proto::Flags& flags) {
  thread_local std::random_device random_device;
  thread_local std::mt19937_64 generator(random_device());
  const String unique = "unique:" + std::to_string(generator()) + "-" +
                        std::to_string(generator());
  return cache::FileCache::Hash(HandledSource(Immutable(unique)), {},
                                CommandLineForSimpleCache(flags),
                                Version(flags.compiler().version()));
}

// static
HandledHash CompilationDaemon::GenerateHash(const base::proto::Flags& flags,
                                            const HandledSource& code,
                                            const ExtraFiles& extra_files) {
  const Version version(flags.compiler().version());
  if (flags.has_key_root()) {
    const HandledSource normalized_code(
        Immutable(NormalizeLineMarkers(code.str.string_copy(), flags)));
    return cache::FileCache::Hash(normalized_code, extra_files,
                                  CommandLineForSimpleCache(flags), version);
  }
  return cache::FileCache::Hash(code, extra_files,
                                CommandLineForSimpleCache(flags), version);
}

bool CompilationDaemon::SetupCompiler(base::proto::Flags* flags,
                                      net::proto::Status* status) const {
  // No flags - filled flags.
  if (!flags || flags->compiler().has_path()) {
    return true;
  }

  auto conf = this->conf();
  auto getCompilerPath = [&conf](String version, String* path) {
    for (const auto& conf_version : conf->versions()) {
      if (conf_version.version() == version) {
        path->assign(conf_version.path());
        return true;
      }
    }
    return false;
  };

  if (!getCompilerPath(flags->compiler().version(),
                       flags->mutable_compiler()->mutable_path())) {
    if (status) {
      status->set_code(net::proto::Status::NO_VERSION);
      status->set_description("Compiler not found: " +
                              flags->compiler().version());
    }
    return false;
  }

  auto getPluginsByCompilerVersion = [&conf](String version,
                                             PluginNameMap& plugin_map) {
    for (const auto& conf_version : conf->versions()) {
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

  if (flags.key_files_size()) {
    String key_files;
    for (const auto& file : flags.key_files()) {
      String hash, error;
      if (!file_hashes_.Hash(GetFullPath(current_dir, file), &hash, &error)) {
        LOG(CACHE_ERROR) << "Failed to hash input file " << file << ": "
                         << error;
        return false;
      }
      key_files += file + ":" + hash + "\n";
    }
    extra_files->emplace(cache::KEY_FILES, std::move(key_files));
  }

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
    const HandledHash& hash, cache::FileCache::Entry* entry) const {
  if (!cache_) {
    return false;
  }
  Counter counter(Metric::SIMPLE_CACHE_LOOKUP_TIME);
  return cache_->Find(hash, entry);
}

bool CompilationDaemon::SearchTimeTrace(const cache::FileCache::Entry& entry,
                                        Immutable* trace) const {
  return cache_ && cache_->FindTimeTrace(entry.time_trace_hash, trace);
}

bool CompilationDaemon::SearchDirectCache(
    const base::proto::Flags& flags, const String& current_dir,
    cache::FileCache::Entry* entry, bool* source_edited,
    bool* source_seen) const {
  auto conf = this->conf();

  DCHECK(conf->has_emitter() && !conf->has_absorber());

  if (!cache_ || !conf->cache().direct()) {
    return false;
  }

  DCHECK(flags.has_input());

  Counter counter(Metric::DIRECT_CACHE_LOOKUP_TIME);

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

  const bool found = cache_->Find(code, extra_files, command_line, version,
                                 current_dir, entry, flags.key_root(),
                                 IncludeSearchDirs(flags, current_dir));

  // The same source with the same flags - regardless of the headers.
  const String source_key =
      base::Hexify((Immutable(input) + "\n"_l + command_line.str + "\n"_l +
                    version.str)
                       .Hash());
  const bool edited = cache_->ReplaceLastSource(
      source_key,
      cache::FileCache::Hash(code, extra_files, command_line, version));

  if (!found) {
    LOG(CACHE_INFO) << "Direct cache miss: " << flags.input()
                    << (edited ? " - the source is edited" : "");
    if (source_edited) {
      *source_edited = edited;
    }
    if (source_seen) {
      const String seen_key = base::Hexify(
          (Immutable(input) + "\n"_l +
           CommandLineForSimpleCache(flags).str + "\n"_l + version.str)
              .Hash());
      *source_seen = !cache_->MarkSeen(seen_key);
    }
    return false;
  }

  return true;
}

void CompilationDaemon::UpdateSimpleCache(
    const cache::string::HandledHash& hash,
    const cache::FileCache::Entry& entry) {
  if (!cache_) {
    return;
  }
  Counter counter(Metric::SIMPLE_CACHE_UPDATE_TIME);
  cache_->Store(hash, entry);
}

void CompilationDaemon::TagEntry(const base::proto::Local& message,
                                 const HandledHash& hash) {
  if (!cache_ || hash.str.empty()) {
    return;
  }
  const String prefix = String(base::kEnvTags) + "=";
  for (const auto& variable : message.env()) {
    if (variable.starts_with(prefix)) {
      List<String> tags;
      base::SplitString<'\n'>(variable.substr(prefix.size()), tags);
      cache_->AddTags(hash, Vector<String>(tags.begin(), tags.end()));
      return;
    }
  }
}

bool CompilationDaemon::HandleTagsRequest(
    net::ConnectionPtr connection, const base::proto::TagsRequest& request) {
  if (!cache_) {
    net::proto::Status status;
    status.set_code(net::proto::Status::BAD_MESSAGE);
    status.set_description("The cache is disabled");
    return connection->ReportStatus(status);
  }

  // The connection lives, until the reply is sent. The client, that is gone,
  // doesn't need it.
  auto needed = [connection] { return !connection->IsPeerGone(); };
  if (request.forget_ghosts()) {
    return cache_->ForgetGhosts(
        [connection](ui64 count) {
          auto report = std::make_unique<base::proto::TagsReport>();
          report->set_removed_ghosts(count);
          connection->SendAsync(std::move(report));
        },
        needed);
  }

  if (request.has_forget()) {
    return cache_->ForgetTag(
        request.forget(), request.dry_run(),
        [connection](const cache::FileCache::ForgetResult& result) {
          auto report = std::make_unique<base::proto::TagsReport>();
          report->set_removed_entries(result.removed_entries);
          report->set_freed_size(result.freed_size);
          report->set_shared_entries(result.shared_entries);
          report->set_linked_entries(result.linked_entries);
          connection->SendAsync(std::move(report));
        },
        needed);
  }

  return cache_->GetTagStats(
      [connection](const List<cache::FileCache::TagStats>& stats,
                   const List<cache::FileCache::TagOverlap>& overlaps) {
        auto report = std::make_unique<base::proto::TagsReport>();
        for (const auto& stat : stats) {
          auto* tag = report->add_tags();
          tag->set_name(stat.tag);
          tag->set_entries(stat.entries);
          tag->set_size(stat.size);
          tag->set_exclusive_entries(stat.exclusive_entries);
          tag->set_exclusive_size(stat.exclusive_size);
          tag->set_linked_size(stat.linked_size);
          tag->set_freeable_size(stat.freeable_size);
          tag->set_lost_entries(stat.lost_entries);
          tag->set_lost_time(stat.lost_time);
        }
        for (const auto& overlap : overlaps) {
          auto* new_overlap = report->add_overlaps();
          new_overlap->set_first(overlap.first);
          new_overlap->set_second(overlap.second);
          new_overlap->set_size(overlap.size);
        }
        connection->SendAsync(std::move(report));
      },
      needed);
}

void CompilationDaemon::RemoveFromCache(const HandledHash& hash) {
  if (cache_) {
    cache_->Remove(hash);
  }
}

void CompilationDaemon::UpdateDirectCache(
    const base::proto::Local* message, const HandledHash& hash,
    const ExtraFiles& extra_files, Immutable deps,
    i64 start_time) {
  auto conf = this->conf();
  DCHECK(conf->has_emitter() && !conf->has_absorber());

  if (!cache_ || !conf->cache().direct()) {
    return;
  }

  const auto& flags = message->flags();
  DCHECK(flags.has_input());

  if (deps.empty()) {
    LOG(CACHE_WARNING) << "Can't update direct cache without deps : "
                       << flags.input();
    return;
  }
  Counter counter(Metric::DIRECT_CACHE_UPDATE_TIME);

  const Version version(flags.compiler().version());
  const auto command_line =
      CommandLineForDirectCache(message->current_dir(), flags);
  const String input_path = GetFullPath(message->current_dir(), flags.input());
  List<String> headers;
  UnhandledSource original_code;

  if (ParseDeps(deps, headers) &&
      base::File::Read(input_path, &original_code.str)) {
    List<String> preprocessed_headers(message->flags().included_files().begin(),
                                      message->flags().included_files().end());
    cache_->Store(original_code, extra_files, command_line, version, headers,
                  preprocessed_headers, message->current_dir(), hash,
                  flags.key_root(),
                  IncludeSearchDirs(flags, message->current_dir()),
                  start_time);
  } else {
    LOG(CACHE_ERROR) << "Failed to parse deps or read input " << input_path;
  }
}

bool CompilationDaemon::Check(const Configuration& conf) const {
  if (!BaseDaemon::Check(conf)) {
    return false;
  }

  for (const auto& version : conf.versions()) {
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

  return true;
}

// static
base::ProcessPtr CompilationDaemon::CreateProcess(
    const base::proto::Flags& flags, ui32 user_id, const Path& cwd_path) {
  DCHECK(flags.compiler().has_path());
  base::ProcessPtr process =
      base::Process::Create(flags.compiler().path(), cwd_path, user_id);
  auto arg_list = CreateArgumentList(flags.other(), flags.non_cached(),
                                     flags.cc_only(), flags.non_direct(),
                                     flags.ignored(), flags.warnings());
  process->AppendArg(arg_list.begin(), arg_list.end());

  process->AppendArg(Immutable(flags.action()));
  for (const auto& plugin : flags.compiler().plugins()) {
    process->AppendArg("-Xclang"_l)
        .AppendArg("-load"_l)
        .AppendArg("-Xclang"_l)
        .AppendArg(Immutable(plugin.path()));
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

  for (const auto& variable : flags.env()) {
    const auto pos = variable.find('=');
    process->AddEnv(variable.substr(0, pos).c_str(),
                    pos == String::npos ? "" : variable.c_str() + pos + 1);
  }

  return process;
}

// static
base::ProcessPtr CompilationDaemon::CreateProcess(
    const base::proto::Flags& flags, const Path& cwd_path) {
  return CreateProcess(flags, base::Process::SAME_UID, cwd_path);
}

}  // namespace daemon
}  // namespace dist_clang
