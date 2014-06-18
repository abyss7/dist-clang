#pragma once

#include <base/aliases.h>
#include <base/string_utils.h>

#include <third_party/libcxx/exported/include/iostream>
#include <third_party/libcxx/exported/include/sstream>
#include <third_party/libcxx/exported/include/vector>
#include <third_party/libcxxabi/exported/include/cxxabi.h>

#include <execinfo.h>

namespace {

using dist_clang::String;

String Demangle(const char* backtrace_symbol) {
  String string = backtrace_symbol;

  auto begin_name = string.find('(');
  if (begin_name == String::npos) {
    return string;
  }
  begin_name++;

  auto end_name = string.find('+', begin_name);
  if (end_name == String::npos) {
    return string;
  }

  String mangled_name = string.substr(begin_name, end_name - begin_name);
  size_t size = 256;
  int status;
  char* demangled_name =
      abi::__cxa_demangle(mangled_name.c_str(), nullptr, &size, &status);
  if (status == 0) {
    auto result = String(demangled_name);
    free(demangled_name);
    return result;
  } else {
    if (demangled_name) {
      free(demangled_name);
    }
    return mangled_name;
  }
}

}  // namespace

namespace dist_clang {
namespace base {

inline void GetStackTrace(ui8 depth, std::vector<String>& strings) {
  using void_ptr = void*;
  UniquePtr<void_ptr[]> buffer(new void_ptr[depth + 1]);

  auto size = backtrace(buffer.get(), depth + 1);
  auto symbols = backtrace_symbols(buffer.get(), size);
  strings.resize(size - 1);
  for (int i = 1; i < size; ++i) {
    strings[i - 1] = Demangle(symbols[i]);
    Replace(strings[i - 1],
            "std::__1::basic_string<char, std::__1::char_traits<char>, "
            "std::__1::allocator<char> >",
            "std::string");
  }
  free(symbols);
}

// Don't use |base::Log| inside |CHECK()| since we always need a stacktrace -
// even if we disable logging at all. Also, potentially, |base::Log| facility
// may use assertions too.

// In tests it's better to throw unhandled exception - not to crash the whole
// binary, but catch the failure.
#if __has_feature(cxx_exceptions)
#define CHECK(expr)                                   \
  if (!(expr)) {                                      \
    std::stringstream ss;                             \
    std::vector<String> strings;                      \
    dist_clang::base::GetStackTrace(62, strings);     \
    ss << "Assertion failed: " << #expr << std::endl; \
    for (const auto& str : strings) {                 \
      ss << "  " << str << std::endl;                 \
    }                                                 \
    throw std::runtime_error(ss.str());               \
  }
#else  // !__has_feature(cxx_exceptions)
#define CHECK(expr)                                   \
  if (!(expr)) {                                      \
    std::stringstream ss;                             \
    std::vector<String> strings;                      \
    dist_clang::base::GetStackTrace(62, strings);     \
    ss << "Assertion failed: " << #expr << std::endl; \
    for (const auto& str : strings) {                 \
      ss << "  " << str << std::endl;                 \
    }                                                 \
    std::cerr << ss.str();                            \
    std::abort();                                     \
  }
#endif  // __has_feature(cxx_exceptions)

#if defined(NDEBUG)
#define DCHECK_O_EVAL(expr) (void)(expr)
#define DCHECK(expr)
#else
#define DCHECK_O_EVAL(expr) CHECK(expr)
#define DCHECK(expr) CHECK(expr)
#endif
#define NOTREACHED() DCHECK(false)

}  // namespace base
}  // namespace dist_clang
