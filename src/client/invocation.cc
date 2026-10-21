#include <client/invocation.h>

#include <base/constants.h>
#include <base/string_utils.h>
#include <client/driver_name.h>

namespace dist_clang {
namespace client {

namespace {

String BaseName(const String& path) {
  return path.substr(path.find_last_of('/') + 1);
}

}  // namespace

String TagsVariable(const String& env_tags, const List<String>& tags) {
  List<String> all_tags;
  base::SplitByWhitespace(env_tags, all_tags);
  all_tags.insert(all_tags.end(), tags.begin(), tags.end());
  return base::JoinString<'\n'>(all_tags.begin(), all_tags.end());
}

Invocation ParseInvocation(const List<String>& argv,
                           const Fn<String()>& find_compiler) {
  Invocation invocation;
  if (argv.empty()) {
    return invocation;
  }

  const String self_name = BaseName(argv.front());
  if (self_name != "dist-clang") {
    const String compiler_path = find_compiler();
    if (compiler_path.empty()) {
      invocation.action = Invocation::ERROR;
      invocation.error = "provide the real compiler via " +
                         String(base::kEnvClangPath);
      return invocation;
    }
    invocation.action = Invocation::COMPILE;
    invocation.args.push_back(compiler_path);
    // The compiler derives some flags from the name of its executable.
    for (auto& arg : DriverNameArgs(self_name)) {
      invocation.args.push_back(std::move(arg));
    }
    invocation.args.insert(invocation.args.end(), std::next(argv.begin()),
                           argv.end());
    return invocation;
  }

  const Vector<String> args(std::next(argv.begin()), argv.end());
  if (args.empty() || args[0] == "--help") {
    return invocation;
  }
  // The commands take no other arguments - the rest isn't the compiler.
  auto Command = [&](Invocation::Action action, size_t size) {
    if (args.size() == size) {
      invocation.action = action;
    }
    return invocation;
  };
  if (args[0] == "--help-config") {
    return Command(Invocation::HELP_CONFIG, 1);
  }
  if (args[0] == "--stop") {
    return Command(Invocation::STOP, 1);
  }
  if (args[0] == "--stats") {
    invocation.raw = args.size() > 1 && args[1] == "--raw";
    return Command(Invocation::STATS, invocation.raw ? 2 : 1);
  }
  if (args[0] == "--tag-stats") {
    return Command(Invocation::TAG_STATS, 1);
  }
  if (args[0] == "--forget-removed") {
    return Command(Invocation::FORGET_REMOVED, 1);
  }
  const String kForgetTag = "--forget-tag=";
  const bool untagged = args[0] == "--forget-untagged";
  if (args[0].starts_with(kForgetTag) || untagged) {
    invocation.tag = untagged ? String() : args[0].substr(kForgetTag.size());
    invocation.force = args.size() > 1 && args[1] == "--force";
    if (invocation.tag.empty() && !untagged) {
      return invocation;
    }
    return Command(Invocation::FORGET_TAG, invocation.force ? 2 : 1);
  }

  size_t first_arg = 0;
  const String kTag = "--tag=";
  for (; first_arg < args.size(); ++first_arg) {
    const String& arg = args[first_arg];
    if (arg == "--restat") {
      invocation.restat = true;
    } else if (arg == "--recache") {
      invocation.recache = true;
    } else if (arg.starts_with(kTag)) {
      // E.g. "$(git branch --show-current)" may give nothing. Any whitespace
      // separates the tags.
      base::SplitByWhitespace(arg.substr(kTag.size()), invocation.tags);
    } else {
      break;
    }
  }
  if (first_arg == args.size()) {
    return invocation;
  }
  invocation.action = Invocation::COMPILE;
  invocation.args.assign(args.begin() + first_arg, args.end());
  return invocation;
}

}  // namespace client
}  // namespace dist_clang
