#pragma once

#include <list>
#include <string>

namespace dist_clang {

namespace proto {
class Flags;
}

namespace client {

class ClangFlagSet {
  public:
    enum Action { COMPILE, LINK, PREPROCESS, UNKNOWN };
    using StringList = std::list<std::string>;

    static Action ProcessFlags(StringList &flags, proto::Flags *message);
};

}  // namespace client
}  // namespace dist_clang
