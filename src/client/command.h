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

class DriverCommand;

class Command {
 public:
  using List = List<UniquePtr<Command>>;

  Command(bool is_clang) : is_clang_(is_clang) {}
  virtual ~Command() {}

  virtual base::ProcessPtr CreateProcess(const String& current_dir,
                                         ui32 user_id) const = 0;
  virtual String GetExecutable() const = 0;
  virtual String RenderAllArgs() const = 0;  // For testing.

  inline bool IsClang() const { return is_clang_; }
  DriverCommand* WEAK_PTR AsDriverCommand();

 private:
  const bool is_clang_;
};

class DriverCommand : public Command {
 public:
  static bool GenerateFromArgs(int argc, const char* const raw_argv[],
                               List& commands);

  // Check |IsClang()| before calling this methods.
  void FillFlags(proto::Flags* flags, const String& clang_path) const;
  virtual base::ProcessPtr CreateProcess(const String& current_dir,
                                         ui32 user_id) const override;

  virtual String GetExecutable() const override;
  virtual String RenderAllArgs() const override;

 private:
  DriverCommand(llvm::opt::InputArgList* arg_list,
                SharedPtr<clang::driver::Compilation> compilation,
                SharedPtr<llvm::opt::OptTable> opt_table,
                SharedPtr<clang::driver::Driver> driver)
      : Command(true),
        arg_list_(arg_list),
        compilation_(compilation),
        opt_table_(opt_table),
        driver_(driver) {}

  DriverCommand(const clang::driver::Command* WEAK_PTR driver_command,
                SharedPtr<clang::driver::Compilation> compilation,
                SharedPtr<clang::driver::Driver> driver)
      : Command(false),
        command_(driver_command),
        compilation_(compilation),
        driver_(driver) {}

  const clang::driver::Command* WEAK_PTR command_ = nullptr;
  UniquePtr<llvm::opt::InputArgList> arg_list_;
  SharedPtr<clang::driver::Compilation> compilation_;
  SharedPtr<llvm::opt::OptTable> opt_table_;

  // For "--resource-dir" and |clang::driver::Command::getExecutable()|.
  SharedPtr<clang::driver::Driver> driver_;
};

class CleanCommand : public Command {
 public:
  CleanCommand(const llvm::opt::ArgStringList& temp_files)
      : Command(false), temp_files_(temp_files) {}

  virtual base::ProcessPtr CreateProcess(const String& current_dir,
                                         ui32 user_id) const override;

  virtual String GetExecutable() const override { return rm_path; }
  virtual String RenderAllArgs() const override;

 private:
  static constexpr const char* rm_path = "/bin/rm";
  const llvm::opt::ArgStringList& temp_files_;
};

}  // namespace client
}  // namespace dist_clang
