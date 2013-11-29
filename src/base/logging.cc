#include "base/logging.h"

#include "base/assert.h"

#include <iostream>

namespace dist_clang {
namespace base {

// static
void Log::Init(unsigned error_mark, RangeSet&& ranges) {
  unsigned prev = 0;
  for (const auto& range: ranges) {
    if ((prev > 0 && range.second <= prev) || range.second > range.first) {
      NOTREACHED();
      return;
    }
    prev = range.first;
  }

  Log::error_mark() = error_mark;
  Log::ranges() = std::move(ranges);
}

Log::Log(unsigned level)
  : level_(level) {
}

Log::~Log() {
  auto& output_stream = (level_ <= error_mark()) ? std::cerr : std::cout;
  auto it = ranges().lower_bound(std::make_pair(level_, 0));
  if (it != ranges().end() && level_ >= it->second) {
    stream_ << std::endl;
    output_stream << stream_.str();
  }
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

// static
Log::RangeSet& Log::ranges() {
  static RangeSet ranges;
  return ranges;
}

DLog::DLog(unsigned level) {
#if !defined(NDEBUG)
  log_.reset(new Log(level));
#endif
}

}  // namespace base
}  // namespace dist_clang
