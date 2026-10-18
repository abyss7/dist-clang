#include <client/driver_name.h>

#include <unistd.h>

namespace dist_clang {
namespace client {

List<String> DriverNameArgs(const String& name) {
  String driver = name;
  // The version, like in "clang++-18".
  auto dash = driver.find_last_of('-');
  if (dash != String::npos && dash + 1 < driver.size() &&
      driver.find_first_not_of("0123456789.", dash + 1) == String::npos) {
    driver.resize(dash);
  }

  // The mode is set always: the real compiler may have another one.
  List<String> args = {driver.ends_with("++") ? "--driver-mode=g++"
                                              : "--driver-mode=gcc"};

  // The target before the name of the driver, like in
  // "aarch64-linux-gnu-clang++": the compiler takes the triple, that has at
  // least the architecture and the system.
  dash = driver.find_last_of('-');
  if (dash != String::npos) {
    const String target = driver.substr(0, dash);
    if (target.find('-') != String::npos) {
      args.push_back("--target=" + target);
    }
  }
  return args;
}

void RemoveOutputs(const List<String>& args) {
  String output;
  bool split_dwarf = false;
  for (auto it = args.begin(); it != args.end(); ++it) {
    if (*it == "-o" && std::next(it) != args.end()) {
      output = *std::next(it);
    } else if (it->starts_with("-o") && it->size() > 2) {
      output = it->substr(2);
    } else if (*it == "-gsplit-dwarf" || *it == "-gsplit-dwarf=split") {
      split_dwarf = true;
    } else if (*it == "-gsplit-dwarf=single" || *it == "-gno-split-dwarf") {
      split_dwarf = false;
    }
  }
  if (output.empty() || output == "-") {
    return;
  }
  unlink(output.c_str());
  if (split_dwarf) {
    const auto dot = output.find_last_of('.');
    const auto slash = output.find_last_of('/');
    const String base = dot != String::npos &&
                                (slash == String::npos || dot > slash)
                            ? output.substr(0, dot)
                            : output;
    unlink((base + ".dwo").c_str());
  }
}

}  // namespace client
}  // namespace dist_clang
