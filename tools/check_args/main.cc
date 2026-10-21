// Checks which compilations from a compilation database can be cached.
//
// Reads commands from stdin: the number of the arguments, the working
// directory and the arguments - each ended with '\0'. Prints the unsupported
// commands with the reasons, and the summary. See check_args.py for the
// convenient wrapper.

#include <base/base.pb.h>
#include <base/string_utils.h>
#include <daemon/driver_args.h>

#include STL(iostream)
#include STL(map)

using namespace dist_clang;

namespace {

// The reason without the data of the command - for the summary.
String Category(const String& reason) {
  for (const char* prefix :
       {"unsupported input", "can't track input file",
        "unexpanded response file", "expected exactly one input, got"}) {
    if (reason.starts_with(prefix)) {
      return String(prefix) + " ...";
    }
  }
  return reason;
}

}  // namespace

int main() {
  ui64 total = 0, supported = 0;
  std::map<String, ui64> reasons;

  String count;
  while (std::getline(std::cin, count, '\0')) {
    String current_dir;
    std::getline(std::cin, current_dir, '\0');
    List<String> args;
    for (ui64 i = base::StringTo<ui64>(count); i > 0; --i) {
      args.emplace_back();
      std::getline(std::cin, args.back(), '\0');
    }
    if (!std::cin || args.size() < 2) {
      continue;
    }
    String command;
    for (const auto& arg : args) {
      command += (command.empty() ? "" : " ") + arg;
    }
    args.pop_front();  // compiler

    ++total;
    String reason, error;
    base::proto::Flags flags;
    if (!daemon::ExpandResponseFiles(args, current_dir, &error)) {
      reason = "response file: " + error;
    } else if (daemon::ParseDriverArgs(args, current_dir, &flags, &reason)) {
      ++supported;
      continue;
    }

    ++reasons[Category(reason)];
    std::cout << "unsupported (" << reason << "): " << command.substr(0, 300)
              << std::endl;
  }

  std::cout << std::endl
            << "Supported " << supported << " of " << total << " commands"
            << std::endl;
  for (const auto& [reason, count] : reasons) {
    std::cout << "  " << count << "\t" << reason << std::endl;
  }

  return 0;
}
