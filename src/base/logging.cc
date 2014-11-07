#include <base/logging.h>

#include <base/assert.h>
#include <base/const_string.h>

#include <third_party/libcxx/exported/include/iostream>
#include <third_party/protobuf/exported/src/google/protobuf/text_format.h>

namespace dist_clang {
namespace base {

// static
void Log::Reset(ui32 error_mark, RangeSet&& ranges) {
  ui32 prev = 0;
  for (const auto& range : ranges) {
    if ((prev > 0 && range.second <= prev) || range.second > range.first) {
      NOTREACHED();
      return;
    }
    prev = range.first;
  }

  Log::error_mark() = error_mark;
  Log::ranges().reset(new RangeSet(std::move(ranges)));
}

Log::Log(ui32 level)
    : level_(level), error_mark_(error_mark()), ranges_(ranges()) {
}

Log::~Log() {
  auto& output_stream = (level_ <= error_mark_) ? std::cerr : std::cout;
  auto it = ranges_->lower_bound(std::make_pair(level_, 0));
  if (it != ranges_->end() && level_ >= it->second) {
    stream_ << std::endl;
    output_stream << stream_.str();
  }

  if (level_ == named_levels::FATAL) {
    exit(1);
  }
}

Log& Log::operator<<(const google::protobuf::Message& info) {
  String str;
  if (google::protobuf::TextFormat::PrintToString(info, &str)) {
    stream_ << str;
  }
  return *this;
}

Log& Log::operator<<(std::ostream& (*func)(std::ostream&)) {
  stream_ << func;
  return *this;
}

Log& Log::operator<<(const Immutable& info) {
  stream_ << String(info);
  return *this;
}

// static
ui32& Log::error_mark() {
  static ui32 error_mark = 0;
  return error_mark;
}

// static
std::shared_ptr<Log::RangeSet>& Log::ranges() {
  static std::shared_ptr<RangeSet> ranges(
      new RangeSet{std::make_pair(named_levels::WARNING, named_levels::FATAL)});
  return ranges;
}

DLog::DLog(ui32 level) {
#if !defined(NDEBUG)
  log_.reset(new Log(level));
#endif
}

}  // namespace base
}  // namespace dist_clang
