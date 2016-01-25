#pragma once

#include <base/aliases.h>
#include <base/logging.h>

#include STL(iostream)
#include STL(sstream)

namespace dist_clang {

namespace base {
void GetStackTrace(ui8 depth, Vector<String>& strings);
}  // namespace base

// In tests it's better to throw unhandled exception - not to crash the whole
// binary, but catch the failure.
#if __has_feature(cxx_exceptions)
#define CHECK(expr)                                                  \
  if (!(expr))                                                       \
  [] {                                                               \
    using namespace dist_clang::base;                                \
    std::stringstream ss;                                            \
    Vector<String> strings;                                          \
    GetStackTrace(62, strings);                                      \
    ss << "Assertion failed: " << #expr << std::endl;                \
    for (size_t i = 1; i < strings.size(); ++i) {                    \
      ss << "  " << strings[i] << std::endl;                         \
    }                                                                \
    if (!std::uncaught_exception()) {                                \
      throw std::runtime_error(ss.str());                            \
    } else {                                                         \
      std::cerr << "Exception is not thrown - are we in destructor?" \
                << std::endl;                                        \
    }                                                                \
    return Log(named_levels::ASSERT);                                \
  }()
#else  // !__has_feature(cxx_exceptions)
#define CHECK(expr)                                    \
  if (!(expr))                                         \
  [] {                                                 \
    using namespace dist_clang::base;                  \
    Vector<String> strings;                            \
    Log log(named_levels::ASSERT);                     \
    GetStackTrace(62, strings);                        \
    log << "Assertion failed: " << #expr << std::endl; \
    for (size_t i = 1; i < strings.size(); ++i) {      \
      log << "  " << strings[i] << std::endl;          \
    }                                                  \
    return log;                                        \
  }()
#endif  // __has_feature(cxx_exceptions)

// There is a trick how to use lambda in expression:
//
//   DCHECK([&]{ return false; }());
//                              ^^
//
#if defined(NDEBUG)
// TODO: investigate why clang can't link with |__builtin_assume()|.
#define DCHECK_O_EVAL(expr) (void)(expr);
#define DCHECK(expr)
#define NOTREACHED() __builtin_unreachable()
#else
#define DCHECK_O_EVAL(expr) CHECK(expr)
#define DCHECK(expr) CHECK(expr)
#define NOTREACHED() DCHECK(false) << "NOTREACHED"
#endif

}  // namespace dist_clang
