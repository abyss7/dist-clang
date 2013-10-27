#pragma once

#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <cxxabi.h>
#include <execinfo.h>

namespace {

::std::pair<::std::string, uint64_t> Demangle(const char* backtrace_symbol) {
  ::std::string string = backtrace_symbol;

  auto begin_name = string.find('(');
  if (begin_name == ::std::string::npos) {
    return ::std::make_pair(string, 0);
  }
  begin_name++;

  auto end_name = string.find('+', begin_name);
  if (end_name == ::std::string::npos) {
    return ::std::make_pair(string, 0);
  }

  ::std::string mangled_name = string.substr(begin_name, end_name - begin_name);
  size_t size = 256;
  int status;
  char* demangled_name =
      ::abi::__cxa_demangle(mangled_name.c_str(), nullptr, &size, &status);
  if (status == 0) {
    auto result = ::std::make_pair(::std::string(demangled_name), 0);
    ::free(demangled_name);
    return result;
  }
  else {
    if (demangled_name) {
      ::free(demangled_name);
    }
    return ::std::make_pair(mangled_name, 0);
  }
}

}  // namespace

namespace dist_clang {
namespace base {

inline void GetStackTrace(size_t depth, ::std::vector<::std::string>& strings) {
  using void_ptr = void*;
  ::std::unique_ptr<void_ptr[]> buffer(new void_ptr[depth + 1]);

  auto size = ::backtrace(buffer.get(), depth + 1);
  auto symbols = ::backtrace_symbols(buffer.get(), size);
  strings.resize(size - 1);
  for (int i = 1; i < size; ++i) {
    strings[i-1] = Demangle(symbols[i]).first;
  }
  ::free(symbols);
}

#define CHECK(expr) \
  if (!(expr)) { \
    ::std::stringstream ss; \
    ::std::vector<::std::string> strings; \
    base::GetStackTrace(5, strings); \
    ss << "Assertion failed: " << #expr << ::std::endl; \
    for (const auto& str: strings) { \
      ss << "  " << str << ::std::endl; \
    } \
    ::std::cerr << ss.str(); \
    ::std::abort(); \
  }

#if defined(NDEBUG)
#  define DCHECK_O_EVAL(expr) (expr)
#  define DCHECK(expr)
#else
#  define DCHECK_O_EVAL(expr) CHECK(expr)
#  define DCHECK(expr) CHECK(expr)
#endif
#define NOTREACHED() DCHECK(false)

}  // namespace base
}  // namespace dist_clang
