#include <base/logging.h>

#include <base/assert.h>
#include <base/const_string.h>

#include <third_party/libcxx/exported/include/iostream>
#include <third_party/protobuf/exported/src/google/protobuf/text_format.h>

#include <syslog.h>

namespace dist_clang {
namespace base {

// static
void Log::SetMode(Mode mode) {
  if (Log::mode() == CONSOLE && mode == SYSLOG) {
    openlog(nullptr, 0, LOG_DAEMON);
  } else if (Log::mode() == SYSLOG && mode == CONSOLE) {
    closelog();
  }

  Log::mode() = mode;
}

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
    : level_(level),
      error_mark_(error_mark()),
      ranges_(ranges()),
      mode_(mode()) {
}

Log::~Log() {
  auto it = ranges_->lower_bound(std::make_pair(level_, 0));
  if (it != ranges_->end() && level_ >= it->second) {
    stream_ << std::endl;

    if (mode_ == CONSOLE) {
      auto& output_stream = (level_ <= error_mark_) ? std::cerr : std::cout;
      output_stream << stream_.str();
    } else if (mode_ == SYSLOG) {
      // FIXME: not really a fair mapping.
      switch (level_) {
        case named_levels::FATAL:
          syslog(LOG_CRIT, "%s", stream_.str().c_str());
          break;

        case named_levels::ERROR:
          syslog(LOG_ERR, "%s", stream_.str().c_str());
          break;

        case named_levels::WARNING:
          syslog(LOG_WARNING, "%s", stream_.str().c_str());
          break;

        case named_levels::INFO:
          syslog(LOG_NOTICE, "%s", stream_.str().c_str());
          break;

        default:
          syslog(LOG_INFO, "%s", stream_.str().c_str());
      }
    }
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

// static
Log::Mode& Log::mode() {
  static Mode mode = CONSOLE;
  return mode;
}

}  // namespace base
}  // namespace dist_clang
