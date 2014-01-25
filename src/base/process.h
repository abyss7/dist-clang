#pragma once

#include "net/base/types.h"

#include <list>
#include <string>

namespace dist_clang {
namespace base {

class Process {
  public:
    enum { UNLIMITED = 0, MAX_ARGS = 1024 };

    explicit Process(const std::string& exec_path,
                     const std::string& cwd_path = std::string());

    Process& AppendArg(const std::string& arg);
    template <class Iterator> Process& AppendArg(Iterator begin, Iterator end);

    // |sec_timeout| specifies timeout in seconds - how long should we wait for
    // another portion of output from child process.
    bool Run(unsigned sec_timeout, std::string* error = nullptr);
    bool Run(unsigned sec_timeout, const std::string& input,
             std::string* error = nullptr);

    inline const std::string& stdout() const;
    inline const std::string& stderr() const;

  private:
    class ScopedDescriptor {
      public:
        ScopedDescriptor(net::fd_t fd);
        ~ScopedDescriptor();

        operator net::fd_t ();
        net::fd_t Release();

      private:
        net::fd_t fd_;
    };

    bool RunChild(int (&out_pipe)[2], int (&err_pipe)[2], int* in_pipe);
    void kill(int pid);

    const std::string exec_path_, cwd_path_;
    std::list<std::string> args_;
    std::string stdout_, stderr_;
    bool killed_;
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
