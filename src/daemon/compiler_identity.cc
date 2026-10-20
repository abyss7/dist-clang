#include <daemon/compiler_identity.h>

#include <base/const_string.h>
#include <base/file/file.h>
#include <base/logging.h>
#include <base/process_impl.h>
#include <base/string_utils.h>
#include <base/temporary_dir.h>

#include <base/using_log.h>

namespace dist_clang {
namespace daemon {

bool CompilerIdentity::Get(const String& path, String* identity,
                           String* error) {
  DCHECK(identity);

  std::error_code ec;
  const auto real_path = std::filesystem::canonical(path, ec);
  if (ec) {
    if (error) {
      error->assign("Can't resolve " + path + ": " + ec.message());
    }
    return false;
  }

  String binary_hash;
  if (!binaries_.Hash(real_path, &binary_hash, error)) {
    return false;
  }

  // The defaults of the compiler may change without the binary - e.g. with a
  // new GCC or a configuration file: they are taken again from time to time.
  // Meanwhile the other compilations use the old identity.
  const String key = path + "\n" + binary_hash;
  bool known = false;
  UniqueLock lock(mutex_);
  for (;;) {
    auto it = identities_.find(key);
    if (it == identities_.end()) {
      identities_[key].taking = true;
      break;
    }
    auto& memo = it->second;
    if (memo.taking) {
      // The compilations of a build start at once: only one takes the
      // identity. If it fails, the next one tries.
      taken_.wait(lock);
      continue;
    }
    if (memo.updating || Clock::now() - memo.time < ttl_) {
      *identity = memo.identity;
      return true;
    }
    memo.updating = true;
    known = true;
    break;
  }
  lock.unlock();

  String new_identity, new_error;
  const bool taken = Take(path, binary_hash, &new_identity, &new_error);

  lock.lock();
  taken_.notify_all();
  auto& memo = identities_[key];
  memo.updating = false;
  memo.taking = false;
  if (!taken) {
    if (known) {
      LOG(WARNING) << "Using the old identity of compiler " << path << ": "
                   << new_error;
      *identity = memo.identity;
      return true;
    }
    identities_.erase(key);
    if (error) {
      *error = new_error;
    }
    return false;
  }
  memo.identity = new_identity;
  memo.time = Clock::now();
  *identity = new_identity;
  return true;
}

bool CompilerIdentity::Take(const String& path, const String& binary_hash,
                            String* identity, String* error) {
  // The dry run shows the effective defaults of the compiler: version, target,
  // include paths, configuration files, etc.
  String output = binary_hash;
  auto DryRun = [&](const Path& dir, const List<Literal>& args) {
    auto process = base::Process::Create(path, dir, base::Process::SAME_UID);
    process->AppendArg("-###"_l).AppendArg("-c"_l);
    for (const auto& arg : args) {
      process->AppendArg(arg);
    }
    process->AppendArg("-o"_l).AppendArg("/dev/null"_l);
    if (!process->Run(60, error)) {
      if (error && error->empty()) {
        error->assign("Dry run of " + path + " failed: " +
                      process->stderr().string_copy());
      }
      return false;
    }
    output += "\n" + process->stdout().string_copy() + "\n" +
              process->stderr().string_copy();
    return true;
  };
  if (!DryRun(Path("/"), {"-x"_l, "c++"_l, "/dev/null"_l})) {
    return false;
  }

  // The language of a C source depends on the driver mode, that may be set by
  // the name of the executable: e.g. "clang++" compiles it as C++. The source
  // must exist, but the temporary directory mustn't change the identity.
  const base::TemporaryDir temp_dir;
  if (!base::File::Write(temp_dir.path() / "x.c", Immutable(), error)) {
    return false;
  }
  const size_t c_output_start = output.size();
  if (!DryRun(temp_dir.path(), {"x.c"_l})) {
    return false;
  }
  String c_output = output.substr(c_output_start);
  output.resize(c_output_start);
  std::error_code ec;
  for (const String& dir :
       {std::filesystem::canonical(temp_dir.path(), ec).string(),
        temp_dir.path().string()}) {
    if (!dir.empty()) {
      base::Replace(c_output, dir.c_str(), "<dir>");
    }
  }
  output += c_output;

  *identity = base::Hexify(Immutable(output).Hash());

  LOG(VERBOSE) << "Identity of compiler " << path << " is " << *identity;
  return true;
}

namespace {

// The flags, that change the include directories of the compiler.
const char* const kIncludeDirsFlags[] = {
    "-stdlib",    "--sysroot",   "-isysroot",       "-target",
    "--target",   "-nostdinc",   "-nostdlibinc",    "-nobuiltininc",
    "--gcc-toolchain", "--gcc-install-dir", "-resource-dir", "-arch",
    "-m32",       "-m64",        "-mx32",
};

// The language of the source for "-x".
String SourceLanguage(const base::proto::Flags& flags) {
  if (!flags.language().empty()) {
    return flags.language();
  }
  const String extension = Path(flags.input()).extension().string();
  if (extension == ".c") {
    return "c";
  }
  if (extension == ".m") {
    return "objective-c";
  }
  if (extension == ".mm") {
    return "objective-c++";
  }
  return "c++";
}

}  // namespace

bool CompilerIdentity::GetIncludeDirs(const String& path,
                                      const String& identity,
                                      const base::proto::Flags& flags,
                                      List<String>* dirs, String* error) {
  DCHECK(dirs);

  List<String> args = {"-x", SourceLanguage(flags)};
  for (const auto* list :
       {&flags.other(), &flags.non_cached(), &flags.cc_only()}) {
    for (const auto& arg : *list) {
      if (!arg.values_size()) {
        continue;
      }
      for (const char* prefix : kIncludeDirsFlags) {
        if (arg.values(0).starts_with(prefix)) {
          args.insert(args.end(), arg.values().begin(), arg.values().end());
          break;
        }
      }
    }
  }

  String key = identity;
  for (const auto& arg : args) {
    key += "\n" + arg;
  }
  {
    std::lock_guard<Mutex> lock(mutex_);
    auto it = include_dirs_.find(key);
    if (it != include_dirs_.end()) {
      *dirs = it->second;
      return true;
    }
  }

  // The verbose preprocessing of nothing shows the search list.
  auto process = base::Process::Create(path, Path("/"), base::Process::SAME_UID);
  for (const auto& arg : args) {
    process->AppendArg(Immutable(arg));
  }
  process->AppendArg("-E"_l)
      .AppendArg("-v"_l)
      .AppendArg("-o"_l)
      .AppendArg("/dev/null"_l)
      .AppendArg("/dev/null"_l);
  if (!process->Run(60, error)) {
    if (error && error->empty()) {
      error->assign("Can't get the include directories of " + path + ": " +
                    process->stderr().string_copy());
    }
    return false;
  }

  *dirs = ParseIncludeDirs(process->stderr().string_copy());
  std::lock_guard<Mutex> lock(mutex_);
  include_dirs_.emplace(key, *dirs);
  return true;
}

// static
List<String> CompilerIdentity::ParseIncludeDirs(const String& output) {
  List<String> lines, dirs;
  base::SplitString<'\n'>(output, lines);
  bool in_list = false;
  for (auto& line : lines) {
    if (line.starts_with("#include <...> search starts here:")) {
      in_list = true;
      continue;
    }
    if (!in_list) {
      continue;
    }
    if (line.starts_with("End of search list.")) {
      break;
    }
    // " /usr/include", " /Library/Frameworks (framework directory)".
    const auto start = line.find_first_not_of(' ');
    if (start == String::npos) {
      continue;
    }
    String dir = line.substr(start);
    const String kFramework = " (framework directory)";
    if (dir.ends_with(kFramework)) {
      dir.resize(dir.size() - kFramework.size());
    }
    dirs.push_back(std::move(dir));
  }
  return dirs;
}

// static
String CompilerIdentity::Resolve(const String& name, const String& current_dir,
                                 const List<String>& env) {
  if (name.find('/') != String::npos) {
    return Path(name).is_absolute() ? name : current_dir + "/" + name;
  }

  String path_variable;
  for (const auto& variable : env) {
    if (variable.starts_with("PATH=")) {
      path_variable = variable.substr(5);
      break;
    }
  }

  List<String> dirs;
  base::SplitString<':'>(path_variable, dirs);
  for (const auto& dir : dirs) {
    const String candidate =
        (dir.empty() ? current_dir
                     : Path(dir).is_absolute() ? dir : current_dir + "/" + dir) +
        "/" + name;
    if (base::File::IsExecutable(candidate)) {
      return candidate;
    }
  }

  return String();
}

}  // namespace daemon
}  // namespace dist_clang
