#pragma once

#include <list>
#include <string>

namespace dist_clang {

namespace proto {
class Flags;
}

namespace client {

class FlagSet {
 public:
  enum Action {
    COMPILE,
    LINK,
    PREPROCESS,
    UNKNOWN
  };
  using StringList = std::list<std::string>;
  using CommandList = std::list<StringList>;

  static Action ProcessFlags(StringList flags, proto::Flags* message);
  static bool ParseClangOutput(const std::string& output, std::string* version,
                               CommandList& args);
};

}  // namespace client
}  // namespace dist_clang
