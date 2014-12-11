#pragma once

#include <base/aliases.h>

#include STL(set)
#include STL(sstream)

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

#pragma push_macro("FATAL")
#pragma push_macro("ERROR")
#pragma push_macro("WARNING")
#pragma push_macro("INFO")
#pragma push_macro("VERBOSE")
#pragma push_macro("TRACE")
#pragma push_macro("CACHE_ERROR")
#pragma push_macro("CACHE_WARNING")
#pragma push_macro("CACHE_INFO")
#pragma push_macro("CACHE_VERBOSE")
#pragma push_macro("DB_ERROR")
#pragma push_macro("DB_WARNING")
#pragma push_macro("DB_INFO")
#pragma push_macro("DB_VERBOSE")

#undef FATAL

#undef ERROR
#undef WARNING
#undef INFO
#undef VERBOSE
#undef TRACE

#undef CACHE_ERROR
#undef CACHE_WARNING
#undef CACHE_INFO
#undef CACHE_VERBOSE

#undef DB_ERROR
#undef DB_WARNING
#undef DB_INFO
#undef DB_VERBOSE

// The |FATAL| is a special value: after LOG(FATAL) the program terminates with
// |exit(1)|.
enum : ui32 {
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

#pragma pop_macro("FATAL")
#pragma pop_macro("ERROR")
#pragma pop_macro("WARNING")
#pragma pop_macro("INFO")
#pragma pop_macro("VERBOSE")
#pragma pop_macro("TRACE")
#pragma pop_macro("CACHE_ERROR")
#pragma pop_macro("CACHE_WARNING")
#pragma pop_macro("CACHE_INFO")
#pragma pop_macro("CACHE_VERBOSE")
#pragma pop_macro("DB_ERROR")
#pragma pop_macro("DB_WARNING")
#pragma pop_macro("DB_INFO")
#pragma pop_macro("DB_VERBOSE")

}  // namespace NamedLevels

class Log {
 public:
  // First value is a right edge of interval, the second - a left edge.
  using RangeSet = std::set<Pair<ui32>>;

  enum Mode {
    CONSOLE,
    SYSLOG,
  };

  // We need a separate method to be able to change mode before daemonizing.
  static void SetMode(Mode mode);

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
    this->operator<<(*it);
    ++it;
    for (; it != info.end(); ++it) {
      stream_ << " ";
      this->operator<<(*it);
    }
    return *this;
  }

  Log& operator<<(std::ostream& (*func)(std::ostream&));  // for |std::endl|

 private:
  static ui32& error_mark();
  static SharedPtr<RangeSet>& ranges();
  static Mode& mode();

  ui32 level_;
  ui32 error_mark_;
  SharedPtr<RangeSet> ranges_;
  std::stringstream stream_;
  Mode mode_ = CONSOLE;
};

}  // namespace base
}  // namespace dist_clang
