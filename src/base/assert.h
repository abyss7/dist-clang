#pragma once

#include <base/aliases.h>

#include STL(iostream)
#include STL(sstream)

namespace dist_clang {

namespace base {
void GetStackTrace(ui8 depth, Vector<String>& strings);
}  // namespace base

// Don't use |base::Log| inside |CHECK()| since we always need a stacktrace -
// even if we disable logging at all.

// In tests it's better to throw unhandled exception - not to crash the whole
// binary, but catch the failure.
#if __has_feature(cxx_exceptions)
#define CHECK(expr)                                                  \
  if (!(expr)) {                                                     \
    std::stringstream ss;                                            \
    Vector<String> strings;                                          \
    dist_clang::base::GetStackTrace(62, strings);                    \
    ss << "Assertion failed: " << #expr << std::endl;                \
    for (const auto& str : strings) {                                \
      ss << "  " << str << std::endl;                                \
    }                                                                \
    if (!std::uncaught_exception()) {                                \
      throw std::runtime_error(ss.str());                            \
    } else {                                                         \
      std::cerr << "Exception is not thrown - are we in destructor?" \
                << std::endl;                                        \
    }                                                                \
  }
#else  // !__has_feature(cxx_exceptions)
#define CHECK(expr)                                   \
  if (!(expr)) {                                      \
    std::stringstream ss;                             \
    Vector<String> strings;                           \
    dist_clang::base::GetStackTrace(62, strings);     \
    ss << "Assertion failed: " << #expr << std::endl; \
    for (const auto& str : strings) {                 \
      ss << "  " << str << std::endl;                 \
    }                                                 \
    std::cerr << ss.str();                            \
    std::abort();                                     \
  }
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
#define NOTREACHED() DCHECK(false)
#endif

}  // namespace dist_clang
