#include <base/assert.h>
#include <base/c_utils.h>
#include <base/constants.h>
#include <base/logging.h>
#include <base/string_utils.h>
#include <client/autostart.h>
#include <client/clang.h>
#include <client/configuration.hh>
#include <client/driver_name.h>
#include <client/invocation.h>
#include <daemon/config_help.h>
#include <net/end_point.h>

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
    "       dist-clang --stop\n"
    "       dist-clang --help-config\n"
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
    "                The same with DC_TAGS in the environment - several\n"
    "                tags are separated by spaces, also in --tag.\n"
    "  --tag-stats   Show the sizes of the tags in the cache.\n"
    "  --forget-tag  Remove the tag from the cache, and the entries, that\n"
    "                only it uses - with --force; otherwise only show them.\n"
    "  --forget-untagged  The same for the entries without tags.\n"
    "  --forget-removed  Forget the records of the removed entries, that\n"
    "                count the losses - see \"ghosts_ttl\" in the config.\n"
    "  --stop        Stop the daemon - e.g. to apply its configuration;\n"
    "                the next compilation starts it again.\n"
    "  --help-config  Describe the configuration of the daemon.\n"
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

  // The ignored signals stay ignored after exec: the compiler gets SIGPIPE,
  // like without the client.
  signal(SIGPIPE, SIG_DFL);
  execvp(argv[0], const_cast<char* const*>(argv.data()));
  std::cerr << "dist-clang: failed to execute " << args.front() << ": "
            << strerror(errno) << std::endl;
  exit(1);
}

}  // namespace
}  // namespace dist_clang

int main(int argc, char* argv[]) {
  using namespace dist_clang;

  signal(SIGPIPE, SIG_IGN);

  client::Configuration configuration;
  const auto& config = configuration.config();
  for (const auto& warning : configuration.warnings()) {
    std::cerr << "dist-clang: warning: " << warning << std::endl;
  }

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

  // Without the check the daemon and the client don't find each other, and
  // the builds go on without the cache silently.
  const String socket_error = net::EndPoint::UnixSocketError(socket_path);

  const auto invocation =
      client::ParseInvocation(List<String>(argv, argv + argc),
                              [&configuration] {
                                return configuration.CompilerPath();
                              });
  if (!socket_error.empty() && invocation.action != client::Invocation::USAGE &&
      invocation.action != client::Invocation::HELP_CONFIG &&
      invocation.action != client::Invocation::ERROR) {
    if (invocation.action != client::Invocation::COMPILE) {
      std::cerr << "dist-clang: " << socket_error << std::endl;
      return 1;
    }
    std::cerr << "dist-clang: warning: " << socket_error
              << " - compiling without the daemon" << std::endl;
    ExecuteLocally(invocation.args);
  }
  switch (invocation.action) {
    case client::Invocation::USAGE:
      std::cerr << kUsage;
      return 1;
    case client::Invocation::HELP_CONFIG:
      std::cout << daemon::kConfigHelp;
      return 0;
    case client::Invocation::ERROR:
      std::cerr << "dist-clang: " << invocation.error << std::endl;
      return 1;
    case client::Invocation::STATS:
      return client::PrintStats(socket_path, config, invocation.raw) ? 0 : 1;
    case client::Invocation::TAG_STATS:
      return client::PrintTagStats(socket_path, config) ? 0 : 1;
    case client::Invocation::STOP:
      return client::StopDaemon(socket_path, config) ? 0 : 1;
    case client::Invocation::FORGET_REMOVED:
      return client::ForgetGhosts(socket_path, config) ? 0 : 1;
    case client::Invocation::FORGET_TAG:
      return client::ForgetTag(socket_path, config, invocation.tag,
                               invocation.force)
                 ? 0
                 : 1;
    case client::Invocation::COMPILE:
      break;
  }

  // The daemon gets the options with the environment.
  if (invocation.restat) {
    setenv("DC_RESTAT", "1", 1);
  }
  if (invocation.recache) {
    setenv("DC_RECACHE", "1", 1);
  }
  if (!invocation.tags.empty()) {
    const String tags = client::TagsVariable(
        String(base::GetEnv(base::kEnvTags, Literal::empty)), invocation.tags);
    setenv(String(base::kEnvTags).c_str(), tags.c_str(), 1);
  }
  const auto& args = invocation.args;

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
