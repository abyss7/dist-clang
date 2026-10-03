// Checks which compilations from a compilation database can be cached.
//
// Reads commands from stdin, one per line: the working directory followed by
// the arguments, separated by '\x1f'. Prints the unsupported commands with
// the reasons, and the summary. See check_args.py for the convenient wrapper.

#include <base/base.pb.h>
#include <daemon/driver_args.h>

#include STL(iostream)
#include STL(map)

using namespace dist_clang;

int main() {
  ui64 total = 0, supported = 0;
  std::map<String, ui64> reasons;

  String line;
  while (std::getline(std::cin, line)) {
    List<String> parts;
    size_t start = 0;
    for (size_t pos; (pos = line.find('\x1f', start)) != String::npos;
         start = pos + 1) {
      parts.push_back(line.substr(start, pos - start));
    }
    parts.push_back(line.substr(start));
    if (parts.size() < 3) {
      continue;
    }

    const String current_dir = parts.front();
    parts.pop_front();  // working directory
    parts.pop_front();  // compiler

    ++total;
    String reason, error;
    base::proto::Flags flags;
    if (!daemon::ExpandResponseFiles(parts, current_dir, &error)) {
      reason = "response file: " + error;
    } else if (daemon::ParseDriverArgs(parts, current_dir, &flags, &reason)) {
      ++supported;
      continue;
    }

    ++reasons[reason];
    std::cout << "unsupported (" << reason << "): " << line.substr(0, 300)
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
