#pragma once

#include <memory>
#include <sstream>

namespace dist_clang {
namespace base {

namespace NamedLevels {

enum {
  FATAL =    0u,
  ERROR =   10u,
  WARNING = 20u,
  INFO =    30u,
};

}  // namespace NamedLevels

class Log {
  public:
    static void Init(unsigned error_mark);

    Log(unsigned level);
    ~Log();

    Log(const Log&) = delete;
    Log(Log&&) = delete;
    Log& operator= (const Log&) = delete;

    template <class T>
    Log& operator<< (const T& info);
    Log& operator<< (std::ostream& (*func)(std::ostream&));

  private:
    static unsigned& error_mark();

    unsigned level_;
    std::stringstream stream_;
};

class DLog {
  public:
    DLog(unsigned level);

  private:
    std::unique_ptr<Log> log_;
};

template <class T>
Log& Log::operator<< (const T& info) {
  stream_ << info;
  return *this;
}

}  // namespace base
}  // namespace dist_clang
