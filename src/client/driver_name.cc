#include <client/driver_name.h>

namespace dist_clang {
namespace client {

List<String> DriverNameArgs(const String& name) {
  String driver = name;
  // The version, like in "clang++-18".
  const auto dash = driver.find_last_of('-');
  if (dash != String::npos && dash + 1 < driver.size() &&
      driver.find_first_not_of("0123456789.", dash + 1) == String::npos) {
    driver.resize(dash);
  }

  // The mode is set always: the real compiler may have another one.
  return {driver.ends_with("++") ? "--driver-mode=g++" : "--driver-mode=gcc"};
}

}  // namespace client
}  // namespace dist_clang
