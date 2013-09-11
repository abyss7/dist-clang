#pragma once

#include <list>
#include <string>

namespace dist_clang {
namespace client {

class ClangFlagSet {
  public:
    enum Action { COMPILE, LINK, UNKNOWN };
    typedef std::list<std::string> string_list;

    static Action ProcessFlags(string_list& args);
};

}  // namespace client
}  // namespace dist_clang
