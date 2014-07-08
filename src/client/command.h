#pragma once

#include <base/aliases.h>

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

  void FillFlags(proto::Flags* flags) const;

  String RenderAllArgs() const;  // For testing.

 private:
  Command(llvm::opt::InputArgList* arg_list,
          SharedPtr<clang::driver::Compilation> compilation,
          SharedPtr<llvm::opt::OptTable> opt_table)
      : arg_list_(arg_list), compilation_(compilation), opt_table_(opt_table) {}

  UniquePtr<llvm::opt::InputArgList> arg_list_;
  SharedPtr<clang::driver::Compilation> compilation_;
  SharedPtr<llvm::opt::OptTable> opt_table_;
};

}  // namespace client
}  // namespace dist_clang
