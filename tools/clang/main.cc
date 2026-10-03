#include <base/assert.h>
#include <base/c_utils.h>
#include <base/constants.h>
#include <base/logging.h>
#include <client/autostart.h>
#include <client/clang.h>
#include <client/configuration.hh>

#include STL(iostream)

#include <signal.h>
#include <unistd.h>

#include <base/using_log.h>

namespace dist_clang {
namespace {

const char kUsage[] =
    "Usage: dist-clang [--restat] <compiler> [args...]\n"
    "       dist-clang --stats\n"
    "\n"
    "  --restat  Don't touch outputs, that don't change: for build systems,\n"
    "            that re-stat outputs, like Ninja with \"restat = 1\".\n"
    "\n"
    "Or make a symlink named like the compiler, e.g. \"clang++\", to "
    "dist-clang.\n";

[[noreturn]] void ExecuteLocally(const List<String>& args) {
  Vector<const char*> argv;
  for (const auto& arg : args) {
    argv.push_back(arg.c_str());
  }
  argv.push_back(nullptr);

  execvp(argv[0], const_cast<char* const*>(argv.data()));
  std::cerr << "dist-clang: failed to execute " << args.front() << ": "
            << strerror(errno) << std::endl;
  exit(1);
}

String BaseName(const String& path) {
  return path.substr(path.find_last_of('/') + 1);
}

}  // namespace
}  // namespace dist_clang

int main(int argc, char* argv[]) {
  using namespace dist_clang;

  signal(SIGPIPE, SIG_IGN);

  client::Configuration configuration;
  const auto& config = configuration.config();

  // It's safe to use |base::Log|, since its internal static objects don't need
  // special destruction on |exec|.
  base::Log::RangeSet ranges;
  for (const auto& range : config.verbosity().levels()) {
    ranges.emplace(range.right(), range.left());
  }
  if (!ranges.empty()) {
    base::Log::Reset(config.verbosity().error_mark(), std::move(ranges));
  }

  const String socket_env =
      String(base::GetEnv(base::kEnvSocketPath, Literal::empty));
  const String socket_path =
      socket_env.empty() ? base::DefaultSocketPath() : socket_env;

  // There are two modes:
  //   - as a launcher: "dist-clang clang++ <args>";
  //   - as the compiler itself: through a symlink "clang++" -> "dist-clang".
  List<String> args;
  const String self_name = BaseName(argv[0]);
  if (self_name == "dist-clang") {
    if (argc < 2 || argv[1] == String("--help")) {
      std::cerr << kUsage;
      return 1;
    }
    if (argv[1] == String("--stats")) {
      return client::PrintStats(socket_path, config) ? 0 : 1;
    }
    int first_arg = 1;
    if (argv[first_arg] == String("--restat")) {
      // The daemon gets it with the environment.
      setenv("DC_RESTAT", "1", 1);
      ++first_arg;
    }
    if (first_arg == argc) {
      std::cerr << kUsage;
      return 1;
    }
    args.assign(argv + first_arg, argv + argc);
  } else {
    if (config.path().empty()) {
      std::cerr << "dist-clang: provide the real compiler via "
                << base::kEnvClangPath << std::endl;
      return 1;
    }
    args.push_back(config.path());
    // The driver mode depends on the name of the executable.
    if (self_name.find("++") != String::npos &&
        BaseName(config.path()).find("++") == String::npos) {
      args.push_back("--driver-mode=g++");
    }
    args.insert(args.end(), argv + 1, argv + argc);
  }

  if (config.disabled()) {
    ExecuteLocally(args);
  }

  // NOTICE: Use separate |DoMain| function to make sure that all local objects
  //         get destructed before the invocation of |exec|.
  switch (client::DoMain(args, socket_path, config)) {
    case client::Result::SUCCESS:
      return 0;
    case client::Result::FAILURE:
      return 1;
    case client::Result::RUN_LOCALLY:
      break;
  }

  if (String(base::GetEnv(base::kEnvNoAutostart, Literal::empty)).empty()) {
    client::StartDaemon(socket_path);
  }

  ExecuteLocally(args);
}
