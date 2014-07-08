#pragma once

#include <base/aliases.h>
#include <base/attributes.h>
#include <base/process.h>

#include <clang/Driver/Compilation.h>
#include <llvm/Option/ArgList.h>

namespace dist_clang {

namespace proto {
class Flags;
}

namespace client {

class Command {
 public:
  static bool GenerateFromArgs(int argc, const char* const raw_argv[],
                               List<Command>& commands);

  inline bool IsClang() const { return !command_; }

  // Check |IsClang()| before calling this methods.
  void FillFlags(proto::Flags* flags, const String& clang_path) const;
  base::ProcessPtr CreateProcess(const String& current_dir, ui32 user_id) const;

  String RenderAllArgs() const;  // For testing.

 private:
  Command(llvm::opt::InputArgList* arg_list,
          SharedPtr<clang::driver::Compilation> compilation,
          SharedPtr<llvm::opt::OptTable> opt_table,
          SharedPtr<clang::driver::Driver> driver)
      : arg_list_(arg_list),
        compilation_(compilation),
        opt_table_(opt_table),
        driver_(driver) {}

  Command(const clang::driver::Command* WEAK_PTR driver_command,
          SharedPtr<clang::driver::Compilation> compilation)
      : command_(driver_command), compilation_(compilation) {}

  const clang::driver::Command* WEAK_PTR command_ = nullptr;
  UniquePtr<llvm::opt::InputArgList> arg_list_;
  SharedPtr<clang::driver::Compilation> compilation_;
  SharedPtr<llvm::opt::OptTable> opt_table_;

  // For "--resource-dir"
  SharedPtr<clang::driver::Driver> driver_;
};

}  // namespace client
}  // namespace dist_clang
