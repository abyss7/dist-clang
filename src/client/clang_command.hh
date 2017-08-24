#pragma once

#include <client/command.hh>

#include <llvm/Option/ArgList.h>
#include <llvm/Option/OptTable.h>

namespace dist_clang {
namespace client {

class ClangCommand : public Command {
 public:
  ClangCommand(llvm::ArrayRef<const char*> args,
               SharedPtr<llvm::opt::OptTable> opts);

  base::ProcessPtr CreateProcess(Immutable current_dir,
                                 ui32 user_id) const override;
  String GetExecutable() const override;
  String RenderAllArgs() const override;
  bool CanFillFlags() const override { return true; }
  bool FillFlags(base::proto::Flags* flags, const String& clang_path,
                 const String& clang_major_version,
                 bool rewrite_includes) const override;

 private:
  const llvm::opt::InputArgList arg_list_;
  SharedPtr<llvm::opt::OptTable> opts_;
};

}  // namespace client
}  // namespace dist_clang
