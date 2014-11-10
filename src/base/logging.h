#pragma once

#include <base/aliases.h>

#include <third_party/libcxx/exported/include/set>
#include <third_party/libcxx/exported/include/sstream>

// The whole log facility should be minimalistic, since it may be used in a
// third-party libraries, like gtest.

namespace google {
namespace protobuf {
class Message;
}
}

namespace dist_clang {
namespace base {

// Since the enum values are defined in the parent's scope - to be able to use
// something like this:
//
//     LOG(ERROR) << "Message";
//
// in any part of code, we need a separate namespace with an enum only, to
// include it with "using namespace NamedLevels;". See using_log.h file.

namespace named_levels {

// The |FATAL| is a special value: after LOG(FATAL) the program terminates with
// |exit(1)|.
enum {
  FATAL = 0u,
  ERROR = 10u,
  WARNING = 20u,
  INFO = 30u,
  VERBOSE = 40u,
  TRACE = 50u,
  CACHE_ERROR = 110u,
  CACHE_WARNING = 120u,
  CACHE_INFO = 130u,
  CACHE_VERBOSE = 140u,
  DB_ERROR = 210u,
  DB_WARNING = 220u,
  DB_INFO = 230u,
  DB_VERBOSE = 240u,
};

}  // namespace NamedLevels

class Log {
 public:
  // First value is a right edge of interval, the second - a left edge.
  using RangeSet = std::set<Pair<ui32>>;

  // Expects, that ranges are already filtered.
  static void Reset(ui32 error_mark, RangeSet&& ranges);

  Log(ui32 level);
  ~Log();

  Log(const Log&) = delete;
  Log(Log&&) = delete;
  Log& operator=(const Log&) = delete;

  template <class T>
  Log& operator<<(const T& info) {
    stream_ << info;
    return *this;
  }

  template <class T>
  Log& operator<<(const List<T>& info) {
    auto it = info.begin();
    stream_ << *it;
    ++it;
    for (; it != info.end(); ++it) {
      stream_ << " " << *it;
    }
    return *this;
  }

  Log& operator<<(const google::protobuf::Message& info);

  Log& operator<<(std::ostream& (*func)(std::ostream&));  // for |std::endl|

  Log& operator<<(const Immutable& info);

 private:
  static ui32& error_mark();
  static std::shared_ptr<RangeSet>& ranges();

  ui32 level_;
  ui32 error_mark_;
  std::shared_ptr<RangeSet> ranges_;
  std::stringstream stream_;
};

// Use this class, if for some reason you can't include "using_log.h".
class DLog {
 public:
  DLog(ui32 level);

 private:
  UniquePtr<Log> log_;
};

}  // namespace base
}  // namespace dist_clang
