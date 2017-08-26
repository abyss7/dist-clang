#pragma once

#include <base/empty_lambda.h>
#include <base/process.h>

#include <sys/types.h>

namespace dist_clang {
namespace base {

class TestProcess : public Process {
 public:
  using OnRunCallback = Fn<bool(ui16, Immutable, String*)>;

  class Factory : public Process::Factory {
   public:
    using OnCreateCallback = Fn<void(TestProcess*)>;

    virtual UniquePtr<Process> Create(const String& exec_path,
                                      const Path& cwd_path, ui32 uid) override;

    inline void CallOnCreate(OnCreateCallback callback) {
      on_create_ = callback;
    }

   private:
    OnCreateCallback on_create_ = EmptyLambda<>();
  };

  virtual bool Run(ui16 sec_timeout, String* error) override;
  virtual bool Run(ui16 sec_timeout, Immutable input, String* error) override;

  inline void CallOnRun(OnRunCallback callback) { on_run_ = callback; }
  inline void CountRuns(Atomic<ui32>* counter) { run_attempts_ = counter; }

  String PrintArgs() const;  // helper for gtest assertions.

 private:
  TestProcess(const String& exec_path, const Path& cwd_path, ui32 uid);

  OnRunCallback on_run_ = EmptyLambda<bool>(false);
  Atomic<ui32>* run_attempts_ = nullptr;
};

}  // namespace base
}  // namespace dist_clang
