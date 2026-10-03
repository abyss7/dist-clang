#include <daemon/compiler_identity.h>

#include <base/const_string.h>
#include <base/file/file.h>
#include <base/logging.h>
#include <base/process_impl.h>
#include <base/string_utils.h>

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

  const String key = path + "\n" + binary_hash;
  {
    std::lock_guard<Mutex> lock(mutex_);
    auto it = identities_.find(key);
    if (it != identities_.end()) {
      *identity = it->second;
      return true;
    }
  }

  // The dry run shows the effective defaults of the compiler: version, target,
  // include paths, configuration files, etc.
  auto process = base::Process::Create(path, Path("/"), base::Process::SAME_UID);
  process->AppendArg("-###"_l)
      .AppendArg("-c"_l)
      .AppendArg("-x"_l)
      .AppendArg("c++"_l)
      .AppendArg("/dev/null"_l)
      .AppendArg("-o"_l)
      .AppendArg("/dev/null"_l);
  if (!process->Run(60, error)) {
    if (error && error->empty()) {
      error->assign("Dry run of " + path + " failed: " +
                    process->stderr().string_copy());
    }
    return false;
  }

  Immutable::Rope rope = {Immutable(binary_hash), process->stdout(),
                          process->stderr()};
  *identity = base::Hexify(Immutable(rope).Hash());

  LOG(VERBOSE) << "Identity of compiler " << path << " is " << *identity;

  std::lock_guard<Mutex> lock(mutex_);
  identities_.emplace(key, *identity);
  return true;
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
