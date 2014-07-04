#pragma once

#include <base/aliases.h>
#include <proto/remote.pb.h>

#include <clang/Driver/Compilation.h>
#include <llvm/Option/ArgList.h>

namespace dist_clang {
namespace client {

class Command {
 public:
  // Since the Protobuf doesn't support move-semantics, we use pointer to
  // consume an object.
  Command(proto::ArgList* arg_list);

  static bool GenerateFromArgs(int argc, const char* const raw_argv[],
                               List<Command>& commands);

  void FillArgs(proto::ArgList* arg_list) const;
  String RenderAllArgs() const;

 private:
  class SimpleArgList : public llvm::opt::ArgList {
   public:
    inline virtual const char* getArgString(unsigned index) const override {
      return strings_[index];
    }

    inline virtual unsigned getNumInputArgStrings() const override {
      return strings_.size();
    }

    inline virtual const char* MakeArgString(llvm::StringRef str) const
        override {
      made_strings_.emplace_back(std::move(str.str()));
      strings_.push_back(made_strings_.back().c_str());
      return strings_.back();
    }

    inline unsigned PutString(const char* str) const {
      strings_.push_back(str);
      return strings_.size() - 1;
    }

   private:
    mutable Vector<const char*> strings_;
    mutable List<String> made_strings_;
  };

  Command(llvm::opt::ArgList* arg_list,
          SharedPtr<clang::driver::Compilation> compilation)
      : arg_list_(arg_list), compilation_(compilation) {}

  UniquePtr<llvm::opt::ArgList> arg_list_;
  UniquePtr<proto::ArgList> proto_;
  SharedPtr<clang::driver::Compilation> compilation_;
};

}  // namespace client
}  // namespace dist_clang
