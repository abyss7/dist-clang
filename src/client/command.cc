#include <client/command.h>

#include <proto/remote.pb.h>

#include <clang/Basic/Diagnostic.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/DriverDiagnostic.h>
#include <clang/Driver/Options.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <llvm/Option/Arg.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

namespace dist_clang {
namespace client {

Command::Command(proto::ArgList* arg_list)
    : arg_list_(new SimpleArgList), proto_(arg_list) {
  // Multiple calls per program are allowed.
  llvm::InitializeAllTargets();

  UniquePtr<llvm::opt::OptTable> opts(clang::driver::createDriverOptTable());

  // Assume incoming arguments are ordered by their original index.
  for (const auto& arg : arg_list->args()) {
    // FIXME: not sure if I have to restore the indicies inside |ArgList| -
    //        if not, then remove ugly casting below.
    auto&& option = opts->getOption(arg.option_id());
    auto* option_name = arg_list_->MakeArgString(option.getPrefixedName());
    auto* new_arg = new llvm::opt::Arg(option, option_name,
                                       arg_list_->getNumInputArgStrings());
    for (const auto& value : arg.values()) {
      new_arg->getValues().push_back(value.c_str());
      static_cast<SimpleArgList*>(arg_list_.get())->PutString(value.c_str());
    }
    arg_list_->append(new_arg);
  }
}

// static
bool Command::GenerateFromArgs(int argc, const char* const raw_argv[],
                               List<Command>& commands) {
  using namespace clang;
  using namespace clang::driver;
  using namespace llvm;
  using DiagPrinter = TextDiagnosticPrinter;

  SmallVector<const char*, 256> argv;
  SpecificBumpPtrAllocator<char> arg_allocator;
  auto&& arg_array = ArrayRef<const char*>(raw_argv, argc);
  llvm::sys::Process::GetArgumentVector(argv, arg_array, arg_allocator);

  // Multiple calls per program are allowed.
  llvm::InitializeAllTargets();

  IntrusiveRefCntPtr<DiagnosticOptions> diag_opts = new DiagnosticOptions;
  DiagPrinter* diag_client = new DiagPrinter(llvm::errs(), &*diag_opts);
  IntrusiveRefCntPtr<DiagnosticIDs> diag_id(new DiagnosticIDs());
  IntrusiveRefCntPtr<clang::DiagnosticsEngine> diags;
  SharedPtr<clang::driver::Compilation> compilation;

  diags = new DiagnosticsEngine(diag_id, &*diag_opts, diag_client);

  std::string path = argv[0];
  Driver driver(path, llvm::sys::getDefaultTargetTriple(), "a.out", *diags);
  compilation.reset(driver.BuildCompilation(argv));

  if (!compilation) {
    return false;
  }

  UniquePtr<opt::OptTable> opts(createDriverOptTable());
  bool result = false;
  const auto& jobs = compilation->getJobs();
  for (auto& job : jobs) {
    if (job->getKind() == Job::CommandClass) {
      result = true;

      auto command = static_cast<clang::driver::Command*>(job);
      auto arg_begin = command->getArguments().begin();
      auto arg_end = command->getArguments().end();

      const unsigned included_flags_bitmask = options::CC1Option;
      unsigned missing_arg_index, missing_arg_count;
      commands.emplace_back(std::move(
          Command(opts->ParseArgs(arg_begin, arg_end, missing_arg_index,
                                  missing_arg_count, included_flags_bitmask),
                  compilation)));
      const auto& arg_list = commands.back().arg_list_;

      // Check for missing argument error.
      if (missing_arg_count) {
        diags->Report(diag::err_drv_missing_argument)
            << arg_list->getArgString(missing_arg_index) << missing_arg_count;
        return false;
      }

      // Issue errors on unknown arguments.
      for (auto it = arg_list->filtered_begin(options::OPT_UNKNOWN),
                end = arg_list->filtered_end();
           it != end; ++it) {
        diags->Report(diag::err_drv_unknown_argument)
            << (*it)->getAsString(*arg_list);
        return false;
      }
    }
  }

  return result;
}

void Command::FillArgs(proto::ArgList* arg_list) const {
  if (arg_list) {
    arg_list->Clear();
    for (const auto& arg : *arg_list_) {
      auto* new_arg = arg_list->add_args();
      new_arg->set_option_id(arg->getOption().getID());
      for (const auto& value : arg->getValues()) {
        new_arg->add_values()->assign(value);
      }
    }
  }
}

String Command::RenderAllArgs() const {
  String result;

  for (const auto& arg : *arg_list_) {
    result += String(" ") + arg->getAsString(*arg_list_);
  }

  return result.substr(1);
}

}  // namespace client
}  // namespace dist_clang
