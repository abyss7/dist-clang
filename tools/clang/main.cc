#include <base/assert.h>
#include <base/c_utils.h>
#include <base/constants.h>
#include <base/logging.h>
#include <client/autostart.h>
#include <client/clang.h>
#include <client/configuration.hh>
#include <client/driver_name.h>

#include STL(iostream)

#include <signal.h>
#include <unistd.h>

#include <base/using_log.h>

namespace dist_clang {
namespace {

const char kUsage[] =
    "Usage: dist-clang [--restat] [--recache] [--tag=<tag>...] <compiler>\n"
    "                  [args...]\n"
    "       dist-clang --stats [--raw]\n"
    "       dist-clang --tag-stats\n"
    "       dist-clang --forget-tag=<tag> [--force]\n"
    "       dist-clang --forget-untagged [--force]\n"
    "       dist-clang --forget-removed\n"
    "\n"
    "  --restat      Don't touch outputs, that don't change: for build\n"
    "                systems, that re-stat outputs, like Ninja with\n"
    "                \"restat = 1\".\n"
    "  --recache     Compile without the lookups in the cache, but store\n"
    "                the result: to measure the build, as without the\n"
    "                cache. The same with DC_RECACHE=1 in the environment,\n"
    "                and for the compilations with -ftime-trace.\n"
    "  --stats       Show the statistics of the daemon; --raw prints them\n"
    "                as \"NAME value\" lines - for scripts.\n"
    "  --tag         Tag the used cache entries, e.g. with the branch of the\n"
    "                sources; may be repeated, the empty tags are ignored.\n"
    "  --tag-stats   Show the sizes of the tags in the cache.\n"
    "  --forget-tag  Remove the tag from the cache, and the entries, that\n"
    "                only it uses - with --force; otherwise only show them.\n"
    "  --forget-untagged  The same for the entries without tags.\n"
    "  --forget-removed  Forget the records of the removed entries, that\n"
    "                count the losses - see \"ghosts_ttl\" in the config.\n"
    "\n"
    "Or make a symlink named like the compiler, e.g. \"clang++\", to "
    "dist-clang.\n";

[[noreturn]] void ExecuteLocally(const List<String>& args) {
  client::RemoveOutputs(args);
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
      const bool raw = argc > 2 && argv[2] == String("--raw");
      return client::PrintStats(socket_path, config, raw) ? 0 : 1;
    }
    if (argv[1] == String("--tag-stats")) {
      return client::PrintTagStats(socket_path, config) ? 0 : 1;
    }
    if (argv[1] == String("--forget-removed") && argc == 2) {
      return client::ForgetGhosts(socket_path, config) ? 0 : 1;
    }
    const String kForgetTag = "--forget-tag=";
    const bool untagged = argv[1] == String("--forget-untagged");
    if (String(argv[1]).starts_with(kForgetTag) || untagged) {
      const String tag =
          untagged ? String() : String(argv[1]).substr(kForgetTag.size());
      const bool force = argc > 2 && argv[2] == String("--force");
      if ((tag.empty() && !untagged) || argc > (force ? 3 : 2)) {
        std::cerr << kUsage;
        return 1;
      }
      return client::ForgetTag(socket_path, config, tag, force) ? 0 : 1;
    }

    // The daemon gets the options with the environment.
    int first_arg = 1;
    const String kTag = "--tag=";
    String tags;
    for (; first_arg < argc; ++first_arg) {
      const String arg = argv[first_arg];
      if (arg == "--restat") {
        setenv("DC_RESTAT", "1", 1);
      } else if (arg == "--recache") {
        setenv("DC_RECACHE", "1", 1);
      } else if (arg.starts_with(kTag)) {
        // E.g. "$(git branch --show-current)" may give nothing.
        const String tag = arg.substr(kTag.size());
        if (!tag.empty() && tag.find('\n') == String::npos) {
          tags += (tags.empty() ? "" : "\n") + tag;
        }
      } else {
        break;
      }
    }
    if (!tags.empty()) {
      setenv(String(base::kEnvTags).c_str(), tags.c_str(), 1);
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
    // The compiler derives some flags from the name of its executable.
    for (auto& arg : client::DriverNameArgs(self_name)) {
      args.push_back(std::move(arg));
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
