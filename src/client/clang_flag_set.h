#pragma once

#include <base/aliases.h>

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
  using CommandList = List<List<String>>;

  static Action ProcessFlags(List<String> flags, proto::Flags* message);
  static bool ParseClangOutput(const String& output, String* version,
                               CommandList& args);
};

}  // namespace client
}  // namespace dist_clang
