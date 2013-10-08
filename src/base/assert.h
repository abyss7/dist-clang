#pragma once

#include <iostream>
#include <sstream>

#include <execinfo.h>

namespace dist_clang {
namespace base {

inline void Assert(bool expr) {
#if !defined(NDEBUG)
  if (!expr) {
    std::string output;
    std::stringstream ss(output);
    const int BUFFER_SIZE = 3;
    void* buffer[BUFFER_SIZE];

    ss << "Assertion failed:" << std::endl;
    auto size = backtrace(buffer, BUFFER_SIZE);
    auto strings = backtrace_symbols(buffer, size);
    for (int i = 0; i < size; ++i) {
      ss << "  " << strings[i] << std::endl;
    }
    free(strings);

    std::cerr << ss.str();
    std::abort();
  }
#endif
}

}  // namespace base
}  // namespace dist_clang
