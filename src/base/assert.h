#pragma once

#include <iostream>
#include <sstream>

#include <cxxabi.h>
#include <execinfo.h>

namespace {

std::pair<std::string, uint64_t> Demangle(const char* backtrace_symbol) {
  std::string string = backtrace_symbol;

  auto begin_name = string.find('(');
  if (begin_name == std::string::npos) {
    return std::make_pair(string, 0);
  }
  begin_name++;

  auto end_name = string.find('+', begin_name);
  if (end_name == std::string::npos) {
    return std::make_pair(string, 0);
  }

  std::string mangled_name = string.substr(begin_name, end_name - begin_name);
  size_t size = 256;
  int status;
  char* demangled_name =
      abi::__cxa_demangle(mangled_name.c_str(), nullptr, &size, &status);
  if (status == 0) {
    auto result = std::make_pair(std::string(demangled_name), 0);
    free(demangled_name);
    return result;
  }
  else {
    if (demangled_name) {
      free(demangled_name);
    }
    return std::make_pair(mangled_name, 0);
  }
}

}  // namespace

namespace dist_clang {
namespace base {

inline void Assert(bool expr) {
#if !defined(NDEBUG)
  if (!expr) {
    std::string output;
    std::stringstream ss(output);
    const int BUFFER_SIZE = 3;
    void* buffer[BUFFER_SIZE + 1];

    ss << "Assertion failed:" << std::endl;
    auto size = backtrace(buffer, BUFFER_SIZE + 1);
    auto strings = backtrace_symbols(buffer, size);
    for (int i = 1; i < size; ++i) {
      ss << "  " << Demangle(strings[i]).first << std::endl;
    }
    free(strings);

    std::cerr << ss.str();
    std::abort();
  }
#endif
}

}  // namespace base
}  // namespace dist_clang
