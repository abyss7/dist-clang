#pragma once

#include "base/testable.h"
#include "gtest/gtest_prod.h"

#include <list>
#include <string>

namespace dist_clang {

namespace client {
FORWARD_TEST(ClientTest, NoInputFile);
FORWARD_TEST(ClientTest, CannotSendMessage);
FORWARD_TEST(ClientTest, CannotReadMessage);
FORWARD_TEST(ClientTest, ReadMessageWithoutStatus);
FORWARD_TEST(ClientTest, ReadMessageWithBadStatus);
FORWARD_TEST(ClientTest, SuccessfulCompilation);
FORWARD_TEST(ClientTest, FailedCompilation);
}  // namespace client

namespace base {

class Process;
class ProcessImpl;
using ProcessPtr = std::unique_ptr<Process>;

class Process: public Testable<Process, ProcessImpl,
                               const std::string&, const std::string&> {
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

  private:
    FRIEND_TEST(client::ClientTest, NoInputFile);
    FRIEND_TEST(client::ClientTest, CannotSendMessage);
    FRIEND_TEST(client::ClientTest, CannotReadMessage);
    FRIEND_TEST(client::ClientTest, ReadMessageWithoutStatus);
    FRIEND_TEST(client::ClientTest, ReadMessageWithBadStatus);
    FRIEND_TEST(client::ClientTest, SuccessfulCompilation);
    FRIEND_TEST(client::ClientTest, FailedCompilation);
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
