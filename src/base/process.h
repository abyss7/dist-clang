#pragma once

#include <iostream>
#include <list>
#include <string>

namespace dist_clang {
namespace base {

class Process {
  public:
    enum { MAX_ARGS = 256 };

    explicit Process(const std::string& exec_path,
                     const std::string& cwd_path = std::string());

    Process& AppendArg(const std::string& arg);
    template <typename ConstIterator>
    Process& AppendArg(ConstIterator begin, ConstIterator end);

    // |sec_timeout| specifies timeout in seconds - how long should we wait for
    // another portion of output from child process, and how long to wait
    // between SIG_KILL and SIG_TERM signals sent to process on error.
    bool Run(unsigned short sec_timeout, std::string* error);

    const std::string& stdout() const;
    const std::string& stderr() const;

  private:
    const std::string exec_path_, cwd_path_;
    std::list<std::string> args_;
    std::string stdout_, stderr_;

    void kill();
};

template<class ConstIterator>
Process& Process::AppendArg(ConstIterator begin, ConstIterator end) {
  args_.insert(args_.end(), begin, end);
  return *this;
}

}  // namespace base
}  // namespace dist_clang
