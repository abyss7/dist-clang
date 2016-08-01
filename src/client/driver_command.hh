#pragma once

#include <base/attributes.h>
#include <client/clang_command.hh>

#include <clang/Driver/Compilation.h>

namespace dist_clang {
namespace client {

class DriverCommand : public Command {
 public:
  DriverCommand(const clang::driver::Command& driver_command,
                SharedPtr<clang::driver::Compilation> compilation,
                SharedPtr<clang::driver::Driver> driver)
      : command_(&driver_command), compilation_(compilation), driver_(driver) {}

  DriverCommand(const clang::driver::Command& driver_command,
                SharedPtr<llvm::opt::OptTable> opts,
                SharedPtr<clang::driver::Compilation> compilation,
                SharedPtr<clang::driver::Driver> driver)
      : command_(&driver_command),
        compilation_(compilation),
        driver_(driver),
        clang_(new ClangCommand(driver_command.getArguments(), opts)) {}

  base::ProcessPtr CreateProcess(Immutable current_dir,
                                 ui32 user_id) const override;
  String GetExecutable() const override;
  String RenderAllArgs() const override;
  inline FillResult FillFlags(
      base::proto::Flags* flags, const String& clang_path,
      const String& clang_major_version) const override {
    if (clang_) {
      return clang_->FillFlags(flags, clang_path, clang_major_version);
    }
    return FillResult::DID_NOT_FILL;
  }

 private:
  const clang::driver::Command* WEAK_PTR command_ = nullptr;
  SharedPtr<clang::driver::Compilation> compilation_;

  // For "--resource-dir" and |GetExecutable()|.
  SharedPtr<clang::driver::Driver> driver_;

  UniquePtr<ClangCommand> clang_;
};

}  // namespace client
}  // namespace dist_clang
