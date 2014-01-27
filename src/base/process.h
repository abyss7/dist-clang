#pragma once

#include "base/testable.h"

#include <list>
#include <string>

namespace dist_clang {
namespace base {

class Process;
class ProcessImpl;
using ProcessPtr = std::unique_ptr<Process>;

class Process: public Testable<Process, ProcessImpl, const std::string&,
                               const std::string&> {
  public:
    enum { UNLIMITED = 0 };

    explicit Process(const std::string& exec_path,
                     const std::string& cwd_path = std::string());
    virtual ~Process() {}

    Process& AppendArg(const std::string& arg);
    template <class Iterator> Process& AppendArg(Iterator begin, Iterator end);

    inline const std::string& stdout() const;
    inline const std::string& stderr() const;

    // |sec_timeout| specifies the timeout in seconds - for how long we should
    // wait for another portion of the output from a child process.
    virtual bool Run(unsigned sec_timeout, std::string* error = nullptr) = 0;
    virtual bool Run(unsigned sec_timeout, const std::string& input,
                     std::string* error = nullptr) = 0;

  protected:
    const std::string exec_path_, cwd_path_;
    std::list<std::string> args_;
    std::string stdout_, stderr_;
};

template<class Iterator>
Process& Process::AppendArg(Iterator begin, Iterator end) {
  args_.insert(args_.end(), begin, end);
  return *this;
}

const std::string& Process::stdout() const {
  return stdout_;
}

const std::string& Process::stderr() const {
  return stderr_;
}

}  // namespace base
}  // namespace dist_clang
