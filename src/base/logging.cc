#include <base/logging.h>

#include <base/const_string.h>

#include STL(iostream)

#if !defined(OS_WIN)
#include <syslog.h>
#endif

#include <base/using_log.h>

namespace dist_clang {
namespace base {

// static
void Log::SetMode(Mode mode) {
#if !defined(OS_WIN)
  if (Log::mode() == CONSOLE && mode == SYSLOG) {
    openlog(nullptr, 0, LOG_DAEMON);
  } else if (Log::mode() == SYSLOG && mode == CONSOLE) {
    closelog();
  }
#else
// TODO: implement syslog alternative on Windows.
#endif  // !defined(OS_WIN)

  Log::mode() = mode;
}

// static
void Log::Reset(ui32 error_mark, RangeSet&& ranges) {
  ui32 prev = 0;
  for (const auto& range : ranges) {
    if ((prev > 0 && range.second <= prev) || range.second > range.first) {
      // FIXME: there should be NOTREACHED(), but it will add dependency on the
      // |assert_*.cc| part of the base target.
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
      mode_(mode()) {}

Log::~Log() {
  auto it = ranges_->lower_bound(std::make_pair(level_, 0));
  if ((it != ranges_->end() && level_ >= it->second) ||
      level_ == named_levels::ASSERT) {
    stream_ << std::endl;

    if (mode_ == CONSOLE) {
      auto& output_stream = (level_ <= error_mark_) ? std::cerr : std::cout;
      output_stream << stream_.str() << std::flush;
    } else if (mode_ == SYSLOG) {
#if !defined(OS_WIN)
      // FIXME: not really a fair mapping.
      switch (level_) {
        case named_levels::FATAL:
        case named_levels::ASSERT:
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
#endif  // !defined(OS_WIN)
    }
  }

  if (level_ == named_levels::FATAL) {
    exit(1);
  }

  if (level_ == named_levels::ASSERT) {
    std::abort();
  }
}

Log& Log::operator<<(std::ostream& (*func)(std::ostream&)) {
  stream_ << func;
  return *this;
}

// static
ui32& Log::error_mark() {
  static ui32 error_mark = 0;
  return error_mark;
}

// static
SharedPtr<Log::RangeSet>& Log::ranges() {
  static SharedPtr<RangeSet> ranges(
      new RangeSet{std::make_pair(named_levels::FATAL, named_levels::FATAL)});
  return ranges;
}

// static
Log::Mode& Log::mode() {
  static Mode mode = CONSOLE;
  return mode;
}

}  // namespace base
}  // namespace dist_clang
