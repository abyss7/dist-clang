#include <client/driver_name.h>

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

}  // namespace client
}  // namespace dist_clang
