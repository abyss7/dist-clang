#include <daemon/driver_args.h>

#include <base/base.pb.h>
#include <base/const_string.h>
#include <base/file/file.h>
#include <base/logging.h>

#include STL(algorithm)
#include STL(string_view)

#include <sys/stat.h>

#include <base/using_log.h>

namespace dist_clang {
namespace daemon {

namespace {

// How the argument affects the compilation - see |base::proto::Flags|.
enum class Class {
  OTHER,
  NON_CACHED,
  CC_ONLY,
  // Only the flags of the dependency file start with "-M" here: the emitter
  // tells them by the prefix - to redirect or to drop them.
  NON_DIRECT,
  IGNORED,
  WARNINGS,
};

// Options, that need a special treatment.
enum class Special {
  NONE,
  OUTPUT,
  LANGUAGE,
  INCLUDE,
  INCLUDE_PCH,
  SYSROOT,
  DEPS_FILE,
  DEPS_TARGET,
  DEPS_QUOTED_TARGET,
  XCLANG,
  COMPILATION_DIR,
  MACRO_PREFIX_MAP,
  DEBUG_PREFIX_MAP,
  FILE_PREFIX_MAP,
  TIME_TRACE,
  UNSUPPORTED,
};

struct Option {
  const char* name;
  Class cls;
  Special special = Special::NONE;
};

// Options, that take a value either as the next argument, or joined with the
// name, like "-I dir" and "-Idir".
const Option kJoinedOrSeparate[] = {
    {"-B", Class::OTHER},
    {"-D", Class::NON_CACHED},
    {"-F", Class::NON_CACHED},
    {"-I", Class::NON_CACHED},
    {"-L", Class::OTHER},
    {"-MF", Class::NON_DIRECT, Special::DEPS_FILE},
    {"-MJ", Class::OTHER, Special::UNSUPPORTED},
    {"-MQ", Class::NON_DIRECT, Special::DEPS_QUOTED_TARGET},
    {"-MT", Class::NON_DIRECT, Special::DEPS_TARGET},
    {"-T", Class::OTHER},
    {"-U", Class::NON_CACHED},
    {"-cxx-isystem", Class::NON_CACHED},
    {"-idirafter", Class::NON_CACHED},
    {"-iframework", Class::NON_CACHED},
    {"-iframeworkwithsysroot", Class::NON_CACHED},
    {"-imacros", Class::NON_CACHED},
    {"-include", Class::NON_CACHED, Special::INCLUDE},
    {"-iprefix", Class::NON_CACHED},
    {"-iquote", Class::NON_CACHED},
    {"-isysroot", Class::NON_CACHED, Special::SYSROOT},
    {"-isystem", Class::NON_CACHED},
    {"-iwithprefix", Class::NON_CACHED},
    {"-iwithprefixbefore", Class::NON_CACHED},
    {"-l", Class::OTHER},
    {"-o", Class::OTHER, Special::OUTPUT},
    {"-u", Class::OTHER},
    {"-x", Class::OTHER, Special::LANGUAGE},
    {"-z", Class::OTHER},
};

// Options, that take a value only as the next argument.
const Option kSeparate[] = {
    {"--config", Class::OTHER, Special::UNSUPPORTED},
    {"--param", Class::OTHER},
    {"--serialize-diagnostics", Class::OTHER, Special::UNSUPPORTED},
    {"--sysroot", Class::NON_CACHED},
    {"-Xassembler", Class::OTHER},
    {"-Xclang", Class::OTHER, Special::XCLANG},
    {"-Xlinker", Class::OTHER},
    // Both pass the flags to the frontend as is.
    {"-Xpreprocessor", Class::OTHER, Special::XCLANG},
    {"-arch", Class::OTHER},
    {"-fdebug-compilation-dir", Class::OTHER, Special::COMPILATION_DIR},
    {"-framework", Class::OTHER},
    {"-gcc-toolchain", Class::OTHER},
    {"-include-pch", Class::OTHER, Special::INCLUDE_PCH},
    {"-index-store-path", Class::OTHER, Special::UNSUPPORTED},
    {"-isystem-after", Class::NON_CACHED},
    {"-ivfsoverlay", Class::OTHER, Special::UNSUPPORTED},
    {"-mllvm", Class::OTHER},
    {"-resource-dir", Class::OTHER},
    {"-stdlib++-isystem", Class::NON_CACHED},
    {"-target", Class::OTHER},
    {"-working-directory", Class::OTHER, Special::UNSUPPORTED},
};

// Options without a value.
const Option kFlags[] = {
    {"-MD", Class::NON_DIRECT},
    {"-MMD", Class::NON_DIRECT},
    {"-MP", Class::NON_DIRECT},
    {"-ftime-trace", Class::IGNORED, Special::TIME_TRACE},
    {"-ftime-trace-verbose", Class::IGNORED},
    {"-nobuiltininc", Class::NON_CACHED},
    {"-nostdinc", Class::NON_CACHED},
    {"-nostdinc++", Class::NON_CACHED},
    {"-nostdlibinc", Class::NON_CACHED},
    {"-undef", Class::NON_CACHED},
};

// Options with a value joined by '=', matched by the prefix.
const Option kJoined[] = {
    {"--config-system-dir=", Class::OTHER, Special::UNSUPPORTED},
    {"--config-user-dir=", Class::OTHER, Special::UNSUPPORTED},
    {"--config=", Class::OTHER, Special::UNSUPPORTED},
    {"--sysroot=", Class::NON_CACHED},
    {"-fdebug-compilation-dir=", Class::OTHER, Special::COMPILATION_DIR},
    {"-ffile-compilation-dir=", Class::OTHER, Special::COMPILATION_DIR},
    {"-fdebug-prefix-map=", Class::OTHER, Special::DEBUG_PREFIX_MAP},
    {"-ffile-prefix-map=", Class::OTHER, Special::FILE_PREFIX_MAP},
    // Changes "__builtin_FILE()" in the compilation too.
    {"-fmacro-prefix-map=", Class::OTHER, Special::MACRO_PREFIX_MAP},
    {"-ftime-trace-granularity=", Class::IGNORED},
    {"-ftime-trace=", Class::IGNORED, Special::TIME_TRACE},
    {"-save-stats=", Class::OTHER, Special::UNSUPPORTED},
    {"-save-temps=", Class::OTHER, Special::UNSUPPORTED},
    {"-working-directory=", Class::OTHER, Special::UNSUPPORTED},
};

// Options, that make the command unsupported: either not a compilation, or
// with outputs we don't know about, or with inputs we don't track.
const char* const kUnsupported[] = {
    "--analyze",
    "--coverage",
    "--precompile",
    "--verbose",
    "-###",
    "-E",
    "-M",
    "-MM",
    "-S",
    "-emit-ast",
    "-fprofile-arcs",
    // The default profiles in the current directory aren't tracked.
    "-fprofile-instr-use",
    "-fprofile-use",
    "-fstack-usage",
    "-fsyntax-only",
    "-ftest-coverage",
    "-save-stats",
    "-save-temps",
    "-v",
};

const char* const kUnsupportedPrefixes[] = {
    "-Wp,",
    "-fcxx-modules",
    "-fimplicit-module",
    "-fmodule-",
    "-fmodules",
    "-fprebuilt-module",
};

// Options with a path to a file, which contents affect the result.
const char* const kInputFilePrefixes[] = {
    "-fauto-profile=",
    "-fbasic-block-sections=list=",
    "-fmemory-profile-use=",
    "-fpass-plugin=",
    "-fplugin=",
    "-fprofile-instr-use=",
    "-fprofile-list=",
    "-fprofile-remapping-file=",
    "-fprofile-sample-use=",
    "-fprofile-use=",
    "-fsanitize-blacklist=",
    "-fsanitize-coverage-allowlist=",
    "-fsanitize-coverage-blacklist=",
    "-fsanitize-coverage-blocklist=",
    "-fsanitize-coverage-ignorelist=",
    "-fsanitize-coverage-whitelist=",
    "-fsanitize-ignorelist=",
    "-fsanitize-system-ignorelist=",
    "-fxray-always-instrument=",
    "-fxray-attr-list=",
    "-fxray-never-instrument=",
};

// Options, that change only the formatting of the diagnostics.
const char* const kDiagnosticsFormat[] = {
    "-fansi-escape-codes",
    "-fcaret-diagnostics",
    "-fdiagnostics-absolute-paths",
    "-fdiagnostics-fixit-info",
    "-fdiagnostics-parseable-fixits",
    "-fdiagnostics-print-source-range-info",
    "-felide-type",
    "-fno-caret-diagnostics",
    "-fno-diagnostics-fixit-info",
    "-fno-elide-type",
    "-fno-show-column",
    "-fno-show-source-location",
    "-fshow-column",
    "-fshow-source-location",
};

const char* const kDiagnosticsFormatPrefixes[] = {
    "-fcaret-diagnostics-max-lines=",
    "-fconstexpr-backtrace-limit=",
    "-fdiagnostics-format=",
    "-fdiagnostics-show-",
    "-ferror-limit=",
    "-fmacro-backtrace-limit=",
    "-fmessage-length=",
    "-fno-diagnostics-show-",
    "-fspell-checking-limit=",
    "-ftemplate-backtrace-limit=",
};

// Environment variables, that affect the result.
const char* const kEnvAffecting[] = {
    "CPATH",
    "CPLUS_INCLUDE_PATH",
    "C_INCLUDE_PATH",
    "COMPILER_PATH",
    "DEVELOPER_DIR",
    "GCC_EXEC_PREFIX",
    "IPHONEOS_DEPLOYMENT_TARGET",
    "MACOSX_DEPLOYMENT_TARGET",
    "OBJCPLUS_INCLUDE_PATH",
    "OBJC_INCLUDE_PATH",
    "SDKROOT",
    "SOURCE_DATE_EPOCH",
    "TVOS_DEPLOYMENT_TARGET",
    "WATCHOS_DEPLOYMENT_TARGET",
};

// Environment variables, that make the command unsupported.
const char* const kEnvUnsupported[] = {
    "CCC_OVERRIDE_OPTIONS",
};

// Languages, which compilation is supported. Languages are from "-x" flag.
// The headers are compiled into the precompiled headers.
const char* const kLanguages[] = {
    "c",
    "c++",
    "c++-header",
    "c-header",
    "objective-c",
    "objective-c++",
    "objective-c++-header",
    "objective-c-header",
};

// The actions of the frontend, that give an object file - the others give
// either nothing, or the modules, which paths aren't tracked.
const char* const kObjectActions[] = {
    "-emit-llvm",
    "-emit-llvm-bc",
    "-emit-llvm-uselists",
    "-emit-obj",
};

// The flags of the frontend, that are known for the precompiled header: the
// others may change the paths in it - e.g. "-isysroot" after the one, that is
// seen.
const char* const kPchFrontendFlags[] = {
    "-emit-pch",
    "-fno-pch-timestamp",
    "-relocatable-pch",
};

// Extensions of the source files, which compilation is supported.
const char* const kExtensions[] = {
    ".C", ".CC", ".CPP", ".CXX", ".c", ".c++", ".cc", ".cp", ".cpp", ".cxx",
    ".m", ".mm",
};

// The output and the input language are rendered separately - to modify them
// for the preprocessing.
bool RenderedSeparately(const Option& option) {
  return option.special == Special::OUTPUT ||
         option.special == Special::LANGUAGE;
}

template <class T, size_t N>
const Option* FindExact(const T (&options)[N], const String& arg) {
  for (const auto& option : options) {
    if (arg == option.name) {
      return &option;
    }
  }
  return nullptr;
}

// Finds the longest option, which name is a prefix of |arg|.
template <class T, size_t N>
const Option* FindPrefix(const T (&options)[N], const String& arg) {
  const Option* result = nullptr;
  size_t result_size = 0;
  for (const auto& option : options) {
    const std::string_view name = option.name;
    if (arg.size() > name.size() && arg.starts_with(name) &&
        name.size() > result_size) {
      result = &option;
      result_size = name.size();
    }
  }
  return result;
}

template <size_t N>
bool MatchesAny(const char* const (&names)[N], const String& arg) {
  return std::any_of(std::begin(names), std::end(names),
                     [&arg](const char* name) { return arg == name; });
}

template <size_t N>
const char* MatchesAnyPrefix(const char* const (&prefixes)[N],
                             const String& arg) {
  for (const char* prefix : prefixes) {
    if (arg.starts_with(prefix)) {
      return prefix;
    }
  }
  return nullptr;
}

// Checked with "clang++-18 -###": they tune the debug info, but don't turn it
// on - unlike all other "-g" flags.
const char* const kDebugInfoTunings[] = {
    "-gz",
    "-gcolumn-info",
    "-gpubnames",
    "-ggnu-pubnames",
    "-gsimple-template-names",
    "-gembed-source",
    "-gstrict-dwarf",
    "-gbtf",
    "-gcodeview",
    "-gdwarf-aranges",
};

String GetFullPath(const String& current_dir, const String& path) {
  return Path(path).is_absolute() ? path : current_dir + "/" + path;
}

String ReplaceExtension(const String& path, const char* extension) {
  return Path(path).replace_extension(extension).string();
}

}  // namespace

String QuoteMakeTarget(const String& target) {
  String result;

  for (size_t i = 0; i < target.size(); ++i) {
    switch (target[i]) {
      case ' ':
      case '\t':
        // Escape the preceding backslashes.
        for (size_t j = i; j > 0 && target[j - 1] == '\\'; --j) {
          result.push_back('\\');
        }
        // Escape the space/tab.
        result.push_back('\\');
        break;
      case '$':
        result.push_back('$');
        break;
      case '#':
        result.push_back('\\');
        break;
      default:
        break;
    }

    result.push_back(target[i]);
  }

  return result;
}

bool ParseDriverArgs(const List<String>& arg_list, const String& current_dir,
                     base::proto::Flags* flags, String* reason) {
  DCHECK(flags);

  auto Unsupported = [reason](const String& why) {
    if (reason) {
      *reason = why;
    }
    return false;
  };

  const Vector<String> args(arg_list.begin(), arg_list.end());

  List<String> inputs;
  String language, output, deps_file;
  List<String> deps_targets;
  bool has_compile_action = false, has_deps = false, has_debug_info = false,
       has_split_dwarf = false, has_compilation_dir = false,
       has_macro_prefix_map = false, has_debug_prefix_map = false;
  // The default depends on the build of the compiler.
  bool records_command_line = true;
  // The own section of the command line - also without the debug info.
  bool records_command_line_section = false;
  // The value of "-ftime-trace=": the file or the directory of the trace.
  String time_trace_value;
  // The remapped prefixes of the paths: for "__FILE__", for the debug info.
  List<String> macro_prefixes, debug_prefixes;
  auto RemappedPrefix = [](const String& map) {
    String prefix =
        Path(map.substr(0, map.find('='))).lexically_normal().string();
    while (prefix.size() > 1 && prefix.back() == '/') {
      prefix.pop_back();
    }
    return prefix;
  };
  bool next_xclang_is_pch = false;
  // The flags of the frontend, that are passed as is.
  List<String> frontend_flags;
  // The precompiled header: its paths are relative to the last sysroot, if
  // it's relocatable.
  bool has_include = false, relocatable_pch = false;
  String sysroot;

  auto AddArgument = [flags](Class cls, ui32 index,
                             std::initializer_list<String> values) {
    base::proto::Argument* argument = nullptr;
    switch (cls) {
      case Class::OTHER:
        argument = flags->add_other();
        break;
      case Class::NON_CACHED:
        argument = flags->add_non_cached();
        break;
      case Class::CC_ONLY:
        argument = flags->add_cc_only();
        break;
      case Class::NON_DIRECT:
        argument = flags->add_non_direct();
        break;
      case Class::IGNORED:
        argument = flags->add_ignored();
        break;
      case Class::WARNINGS:
        argument = flags->add_warnings();
        break;
    }
    argument->set_index(index);
    for (const auto& value : values) {
      argument->add_values(value);
    }
  };

  // Handles the option with a value. Returns |false| if the command becomes
  // unsupported.
  auto HandleValue = [&](const Option& option, const String& value) {
    switch (option.special) {
      case Special::NONE:
        return true;
      case Special::UNSUPPORTED:
        return Unsupported(String("unsupported flag ") + option.name);
      case Special::OUTPUT:
        output = value;
        return true;
      case Special::LANGUAGE:
        // The language applies only to the following inputs.
        if (!inputs.empty()) {
          return Unsupported("input language after input");
        }
        language = value;
        return true;
      case Special::INCLUDE:
        // The compiler may implicitly use a precompiled header instead of the
        // included one - it doesn't matter: the preprocessed source contains
        // the original header anyway.
        has_include = true;
        return true;
      case Special::INCLUDE_PCH:
        flags->add_included_files(value);
        return true;
      case Special::DEPS_FILE:
        deps_file = value;
        return true;
      case Special::DEPS_TARGET:
        deps_targets.push_back(value);
        return true;
      case Special::DEPS_QUOTED_TARGET:
        deps_targets.push_back(QuoteMakeTarget(value));
        return true;
      case Special::SYSROOT:
        sysroot = value;
        return true;
      case Special::XCLANG:
        frontend_flags.push_back(value);
        if (value == "-relocatable-pch") {
          relocatable_pch = true;
        }
        if (next_xclang_is_pch) {
          next_xclang_is_pch = false;
          flags->add_included_files(value);
        } else if (value == "-include-pch") {
          next_xclang_is_pch = true;
        }
        return true;
      case Special::COMPILATION_DIR:
        has_compilation_dir = true;
        return true;
      case Special::MACRO_PREFIX_MAP:
        has_macro_prefix_map = true;
        macro_prefixes.push_back(RemappedPrefix(value));
        return true;
      case Special::DEBUG_PREFIX_MAP:
        has_debug_prefix_map = true;
        debug_prefixes.push_back(RemappedPrefix(value));
        return true;
      case Special::FILE_PREFIX_MAP:
        has_macro_prefix_map = true;
        has_debug_prefix_map = true;
        macro_prefixes.push_back(RemappedPrefix(value));
        debug_prefixes.push_back(RemappedPrefix(value));
        return true;
      case Special::TIME_TRACE:
        flags->set_time_trace(true);
        time_trace_value = value;
        return true;
    }

    NOTREACHED();
    return false;
  };

  for (size_t i = 0; i < args.size(); ++i) {
    const String& arg = args[i];
    const ui32 index = i + 1;

    if (arg.empty()) {
      return Unsupported("empty argument");
    }

    if (arg[0] != '-' || arg == "-") {
      if (arg == "-") {
        return Unsupported("input from stdin");
      }
      if (arg[0] == '@') {
        return Unsupported("unexpanded response file " + arg);
      }
      inputs.push_back(arg);
      continue;
    }

    if (arg == "-c") {
      has_compile_action = true;
      continue;
    }

    if (MatchesAny(kUnsupported, arg)) {
      return Unsupported("unsupported flag " + arg);
    }
    if (auto* prefix = MatchesAnyPrefix(kUnsupportedPrefixes, arg)) {
      return Unsupported(String("unsupported flag ") + prefix);
    }

    if (auto* option = FindExact(kFlags, arg)) {
      if (arg == "-MD" || arg == "-MMD") {
        has_deps = true;
        // The direct cache needs all headers - also the system ones, that
        // "-MMD" leaves out: their changes would give the stale objects. The
        // build system gets them too.
        AddArgument(option->cls, index, {"-MD"});
        continue;
      }
      if (option->special == Special::TIME_TRACE) {
        flags->set_time_trace(true);
        time_trace_value.clear();
      }
      AddArgument(option->cls, index, {arg});
      continue;
    }

    // Options with a value as the next argument.
    const Option* option = FindExact(kSeparate, arg);
    if (!option) {
      option = FindExact(kJoinedOrSeparate, arg);
    }
    if (option) {
      if (i + 1 == args.size()) {
        return Unsupported("no value for " + arg);
      }
      const String& value = args[++i];
      if (!HandleValue(*option, value)) {
        return false;
      }
      if (!RenderedSeparately(*option)) {
        AddArgument(option->cls, index, {arg, value});
      }
      continue;
    }

    // Options with a joined value.
    if (auto* prefix = MatchesAnyPrefix(kInputFilePrefixes, arg)) {
      const String path = arg.substr(strlen(prefix));
      if (!base::File::IsFile(GetFullPath(current_dir, path))) {
        return Unsupported("can't track input file " + arg);
      }
      flags->add_key_files(path);
      AddArgument(Class::OTHER, index, {arg});
      continue;
    }

    option = FindPrefix(kJoined, arg);
    if (!option && !arg.starts_with("-obj")) {
      option = FindPrefix(kJoinedOrSeparate, arg);
    }
    if (option) {
      const String value = arg.substr(strlen(option->name));
      if (!HandleValue(*option, value)) {
        return false;
      }
      if (!RenderedSeparately(*option)) {
        AddArgument(option->cls, index, {arg});
      }
      continue;
    }

    // The color of the diagnostics.
    if (arg == "-fcolor-diagnostics" || arg == "-fdiagnostics-color" ||
        arg == "-fdiagnostics-color=always") {
      flags->set_color_diagnostics(true);
      AddArgument(Class::NON_DIRECT, index, {arg});
      continue;
    }
    if (arg == "-fno-color-diagnostics" || arg == "-fno-diagnostics-color" ||
        arg.starts_with("-fdiagnostics-color=")) {
      flags->set_color_diagnostics(false);
      AddArgument(Class::NON_DIRECT, index, {arg});
      continue;
    }
    if (MatchesAny(kDiagnosticsFormat, arg) ||
        MatchesAnyPrefix(kDiagnosticsFormatPrefixes, arg)) {
      AddArgument(Class::NON_DIRECT, index, {arg});
      continue;
    }

    // Warnings - but not the options for the assembler and the linker. The
    // errors from the warnings, and the warnings of the backend, aren't seen
    // by the recheck without the code generation: they are a part of the key.
    if ((arg.starts_with("-W") && !arg.starts_with("-Wa,") &&
         !arg.starts_with("-Wl,")) ||
        arg == "-w" || arg == "-pedantic" || arg == "-pedantic-errors") {
      const bool affects_backend =
          arg.starts_with("-Werror") || arg.starts_with("-Wno-error") ||
          arg.starts_with("-Wframe-larger-than") ||
          arg.starts_with("-Wno-frame-larger-than") ||
          arg == "-Wattribute-warning" || arg == "-Wno-attribute-warning" ||
          arg == "-Wbackend-plugin" || arg == "-Wno-backend-plugin";
      AddArgument(affects_backend ? Class::OTHER : Class::WARNINGS, index,
                  {arg});
      continue;
    }

    // The debug info contains the current directory, and the name of the split
    // DWARF file.
    if (arg.starts_with("-g")) {
      if (arg == "-g0" || arg == "-ggdb0") {
        has_debug_info = false;
      } else if (arg == "-gsplit-dwarf" || arg == "-gsplit-dwarf=split") {
        has_split_dwarf = true;
      } else if (arg == "-gsplit-dwarf=single" || arg == "-gno-split-dwarf") {
        has_split_dwarf = false;
      } else if (arg == "-grecord-command-line" ||
                 arg == "-grecord-gcc-switches") {
        records_command_line = true;
      } else if (arg == "-gno-record-command-line" ||
                 arg == "-gno-record-gcc-switches") {
        records_command_line = false;
      } else if (!arg.starts_with("-gno-") &&
                 !MatchesAnyPrefix(kDebugInfoTunings, arg)) {
        has_debug_info = true;
      }
    }

    if (arg == "-relocatable-pch" || arg == "--relocatable-pch") {
      relocatable_pch = true;
    }

    if (arg == "-frecord-command-line" || arg == "-frecord-gcc-switches") {
      records_command_line_section = true;
    } else if (arg == "-fno-record-command-line" ||
               arg == "-fno-record-gcc-switches") {
      records_command_line_section = false;
    }

    // By default all other flags affect everything.
    AddArgument(Class::OTHER, index, {arg});
  }

  if (!has_compile_action) {
    return Unsupported("not a compilation");
  }

  if (inputs.size() != 1) {
    return Unsupported("expected exactly one input, got " +
                       std::to_string(inputs.size()));
  }

  const String& input = inputs.front();
  if (!language.empty()) {
    if (!MatchesAny(kLanguages, language)) {
      return Unsupported("unsupported language " + language);
    }
    flags->set_language(language);
  } else if (!MatchesAny(kExtensions, Path(input).extension().string())) {
    return Unsupported("unsupported input " + input);
  }

  bool emits_pch = false, has_pch_timestamp = true;
  for (const auto& value : frontend_flags) {
    if (value == "-emit-pch") {
      emits_pch = true;
    } else if (value == "-fno-pch-timestamp") {
      has_pch_timestamp = false;
    } else if (value.starts_with("-emit-") &&
               !MatchesAny(kObjectActions, value)) {
      return Unsupported("unsupported frontend action " + value);
    }
  }
  if (language.ends_with("-header") || emits_pch) {
    for (const auto& value : frontend_flags) {
      if (!MatchesAny(kPchFrontendFlags, value)) {
        return Unsupported(
            "unsupported flag of the precompiled header -Xclang " + value);
      }
    }
    // The users of the header would check the times of the headers - which
    // are another ones, when it's from the cache.
    if (has_pch_timestamp) {
      return Unsupported(
          "precompiled header without -Xclang -fno-pch-timestamp");
    }
    if (has_include || flags->included_files_size()) {
      return Unsupported(
          "precompiled header with -include: it may be on top of another one");
    }
    // The cache of the precompiled headers is only the direct one.
    if (!has_deps) {
      return Unsupported("precompiled header without the dependency file");
    }
    if (output.empty()) {
      return Unsupported("precompiled header without the output");
    }
    flags->set_pch(true);
    if (relocatable_pch && !sysroot.empty()) {
      flags->set_pch_sysroot(sysroot);
    }
  }

  if (output.empty()) {
    output = Path(input).filename().replace_extension(".o").string();
  } else if (output == "-") {
    return Unsupported("output to stdout");
  }

  flags->set_input(input);
  flags->set_output(output);
  flags->set_action("-c");

  if (has_deps) {
    flags->set_deps_file(deps_file.empty() ? ReplaceExtension(output, ".d")
                                           : deps_file);
    if (deps_targets.empty()) {
      flags->set_deps_target(QuoteMakeTarget(output));
    } else {
      String target;
      for (const auto& deps_target : deps_targets) {
        target += (target.empty() ? "" : " ") + deps_target;
      }
      flags->set_deps_target(target);
    }
  }

  // The coverage mapping has the current directory too - without the debug
  // info, and with "-fdebug-compilation-dir".
  bool has_coverage_mapping = false, has_coverage_dir = false;
  for (const auto& arg : args) {
    if (arg == "-fcoverage-mapping") {
      has_coverage_mapping = true;
    } else if (arg == "-fno-coverage-mapping") {
      has_coverage_mapping = false;
    } else if (arg.starts_with("-ffile-compilation-dir=") ||
               arg.starts_with("-fcoverage-compilation-dir=")) {
      has_coverage_dir = true;
    }
  }
  const bool coverage_has_cwd = has_coverage_mapping && !has_coverage_dir;

  // The paths of the sources get into the result through the "__FILE__" macro
  // and the debug info - unless they are remapped.
  flags->set_relocatable(has_macro_prefix_map && !coverage_has_cwd &&
                         (!has_debug_info ||
                          (has_compilation_dir && has_debug_prefix_map)));
  // The prefixes, that are removed from all paths of the result.
  HashSet<String> seen;
  for (const auto& prefix : macro_prefixes) {
    if ((!has_debug_info ||
         std::find(debug_prefixes.begin(), debug_prefixes.end(), prefix) !=
             debug_prefixes.end()) &&
        seen.insert(prefix).second) {
      flags->add_remapped_prefixes(prefix);
    }
  }

  if ((has_debug_info && !has_compilation_dir) || coverage_has_cwd) {
    flags->add_key_extra("cwd:" + current_dir);
  }
  flags->set_records_command_line(
      records_command_line_section || (has_debug_info && records_command_line));
  if (has_debug_info) {
    if (has_split_dwarf) {
      flags->set_dwo_file(ReplaceExtension(output, ".dwo"));
      flags->add_key_extra("dwo:" + flags->dwo_file());
    }
  }

  // Like the driver: next to the output, or into the given file - or into the
  // given directory with the name of the output.
  if (flags->time_trace()) {
    String trace_file = ReplaceExtension(output, ".json");
    if (!time_trace_value.empty()) {
      std::error_code ec;
      if (std::filesystem::is_directory(
              GetFullPath(current_dir, time_trace_value), ec)) {
        trace_file =
            (Path(time_trace_value) / Path(trace_file).filename()).string();
      } else {
        trace_file = time_trace_value;
      }
    }
    flags->set_time_trace_file(trace_file);
  }

  return true;
}

bool IsDiagnosticsFormatFlag(const String& arg) {
  return MatchesAny(kDiagnosticsFormat, arg) ||
         MatchesAnyPrefix(kDiagnosticsFormatPrefixes, arg);
}

bool ParseEnvironment(const List<String>& env, base::proto::Flags* flags,
                      String* reason) {
  DCHECK(flags);

  List<String> key_parts;
  for (const auto& variable : env) {
    const auto pos = variable.find('=');
    const String name = variable.substr(0, pos);

    if (MatchesAny(kEnvUnsupported, name)) {
      if (reason) {
        *reason = "unsupported environment variable " + name;
      }
      return false;
    }

    if (MatchesAny(kEnvAffecting, name)) {
      key_parts.push_back("env:" + variable);
    }

    flags->add_env(variable);
  }

  key_parts.sort();
  for (const auto& part : key_parts) {
    flags->add_key_extra(part);
  }

  // The compiler takes the current directory for the result from PWD.
  for (auto& part : *flags->mutable_key_extra()) {
    if (part.starts_with("cwd:")) {
      part = "cwd:" + CompilerCurrentDir(*flags, part.substr(4));
    }
  }

  return true;
}

String CompilerCurrentDir(const base::proto::Flags& flags,
                          const String& current_dir) {
  String pwd;
  for (const auto& variable : flags.env()) {
    if (variable.starts_with("PWD=")) {
      pwd = variable.substr(4);
    }
  }
  if (!pwd.starts_with('/') || pwd == current_dir) {
    return current_dir;
  }
  struct stat pwd_st, cwd_st;
  if (stat(pwd.c_str(), &pwd_st) == 0 &&
      stat(current_dir.c_str(), &cwd_st) == 0 &&
      pwd_st.st_dev == cwd_st.st_dev && pwd_st.st_ino == cwd_st.st_ino) {
    return pwd;
  }
  return current_dir;
}

namespace {

// Splits the contents of a response file like GCC does.
List<String> TokenizeResponseFile(const String& content) {
  List<String> tokens;
  String token;
  bool has_token = false;
  char quote = 0;

  for (size_t i = 0; i < content.size(); ++i) {
    const char c = content[i];

    if (c == '\\' && i + 1 < content.size()) {
      token.push_back(content[++i]);
      has_token = true;
    } else if (quote) {
      if (c == quote) {
        quote = 0;
      } else {
        token.push_back(c);
      }
    } else if (c == '"' || c == '\'') {
      quote = c;
      has_token = true;
    } else if (isspace(c)) {
      if (has_token) {
        tokens.push_back(std::move(token));
        token.clear();
        has_token = false;
      }
    } else {
      token.push_back(c);
      has_token = true;
    }
  }

  if (has_token) {
    tokens.push_back(std::move(token));
  }

  return tokens;
}

}  // namespace

bool ExpandResponseFiles(List<String>& args, const String& current_dir,
                         String* error) {
  // Limit the number of all expansions - also against the cyclic files.
  ui32 expansions = 0;

  for (auto it = args.begin(); it != args.end();) {
    if (it->size() < 2 || (*it)[0] != '@') {
      ++it;
      continue;
    }

    if (++expansions > 100) {
      if (error) {
        error->assign("too many response files");
      }
      return false;
    }

    Immutable content;
    if (!base::File::Read(GetFullPath(current_dir, it->substr(1)), &content,
                          error)) {
      return false;
    }

    auto tokens = TokenizeResponseFile(content.string_copy());
    it = args.erase(it);
    // Expand nested response files from the first inserted token.
    it = args.insert(it, tokens.begin(), tokens.end());
  }

  return true;
}

}  // namespace daemon
}  // namespace dist_clang
