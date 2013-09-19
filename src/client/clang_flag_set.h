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
    typedef std::list<std::string> string_list;

    static Action ProcessFlags(string_list &flags, proto::Flags *message);
};

}  // namespace client
}  // namespace dist_clang
