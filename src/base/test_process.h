#pragma once

#include "base/empty_lambda.h"
#include "base/process.h"

#include <functional>
#include <sys/types.h>

namespace dist_clang {
namespace base {

class TestProcess: public Process {
  public:
    using OnRunCallback =
        std::function<bool(unsigned, const std::string&, std::string*)>;

    class Factory: public Process::Factory {
      public:
        using OnCreateCallback = std::function<void(TestProcess*)>;

        virtual std::unique_ptr<Process> Create(
            const std::string& exec_path,
            const std::string& cwd_path) override;

        inline void CallOnCreate(OnCreateCallback callback);

      private:
        OnCreateCallback on_create_ = EmptyLambda<>();
    };

    virtual bool Run(unsigned sec_timeout, std::string *error) override;
    virtual bool Run(unsigned sec_timeout, const std::string& input,
                     std::string *error) override;

    inline void CallOnRun(OnRunCallback callback);
    inline void CountRuns(uint* counter);

  private:
    TestProcess(const std::string& exec_path, const std::string& cwd_path);

    OnRunCallback on_run_ = EmptyLambda<bool>(false);
    uint* run_attempts_ = nullptr;
};

void TestProcess::Factory::CallOnCreate(OnCreateCallback callback) {
  on_create_ = callback;
}

void TestProcess::CallOnRun(OnRunCallback callback) {
  on_run_ = callback;
}

void TestProcess::CountRuns(uint *counter) {
  run_attempts_ = counter;
}

}  // namespace base
}  // namespace dist_clang
