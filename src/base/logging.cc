#include "base/logging.h"

#include <iostream>

namespace dist_clang {
namespace base {

// static
void Log::Init(unsigned error_mark) {
  Log::error_mark() = error_mark;
}

Log::Log(unsigned level)
  : level_(level) {
}

Log::~Log() {
  auto& output_stream = (level_ <= error_mark()) ? std::cerr : std::cout;
  stream_ << std::endl;
  output_stream << stream_.str();
}

Log& Log::operator<< (std::ostream& (*func)(std::ostream&)) {
  stream_ << func;
  return *this;
}

// static
unsigned& Log::error_mark() {
  static unsigned error_mark = 0;
  return error_mark;
}

DLog::DLog(unsigned level) {
#if !defined(NDEBUG)
  log_.reset(new Log(level));
#endif
}

}  // namespace base
}  // namespace dist_clang
