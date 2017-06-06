#include <client/command.hh>

#include <base/logging.h>
#include <client/clang_command.hh>
#include <client/clean_command.hh>
#include <client/driver_command.hh>

#include <clang/Basic/VirtualFileSystem.h>
#include <clang/Driver/Action.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/Options.h>
#include <clang/Frontend/TextDiagnosticBuffer.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/TargetSelect.h>

#include <base/using_log.h>

namespace dist_clang {

namespace {

void DumpDiagnosticBuffer(const clang::TextDiagnosticBuffer* buffer) {
  for (auto it = buffer->note_begin(); it != buffer->note_end(); ++it) {
    LOG(INFO) << "Clang parser note: " << it->second;
  }

  for (auto it = buffer->warn_begin(); it != buffer->warn_end(); ++it) {
    LOG(WARNING) << "Clang parser warning: " << it->second;
  }

  for (auto it = buffer->err_begin(); it != buffer->err_end(); ++it) {
    LOG(WARNING) << "Clang parser error: " << it->second;
  }
}

}  // namespace

namespace client {

// static
bool Command::GenerateFromArgs(int argc, const char* const raw_argv[],
                               List& commands) {
  using namespace clang;
  using namespace clang::driver;
  using namespace llvm;

  // FIXME: check that there is no unnecessery string copying.
  SmallVector<const char*, 256> argv;
  SpecificBumpPtrAllocator<char> arg_allocator;
  auto&& arg_array = ArrayRef<const char*>(raw_argv, argc);
  llvm::sys::Process::GetArgumentVector(argv, arg_array, arg_allocator);

  llvm::InitializeAllTargets();  // Multiple calls per program are allowed.

  IntrusiveRefCntPtr<DiagnosticOptions> diag_opts = new DiagnosticOptions;
  TextDiagnosticBuffer* diag_client = new TextDiagnosticBuffer;
  IntrusiveRefCntPtr<DiagnosticIDs> diag_id(new DiagnosticIDs());
  IntrusiveRefCntPtr<clang::DiagnosticsEngine> diags =
      new DiagnosticsEngine(diag_id, &*diag_opts, diag_client);

  SharedPtr<Compilation> compilation;
  SharedPtr<Driver> driver;

  String path = argv[0];
  String first_arg = argv[1];

  if (first_arg == "-cc1") {
    // Don't create the driver - it will fail to parse internal args.
    SharedPtr<opt::OptTable> opts(createDriverOptTable());
    if (std::regex_match(argv[0], std::regex(".*/?clang(\\+\\+)?"))) {
      auto* command = new ClangCommand(arg_array.slice(1), opts);

      if (diags->hasErrorOccurred()) {
        delete command;
        return false;
      }

      commands.emplace_back(command);
      return true;
    }
  }

  driver.reset(new Driver(path, llvm::sys::getDefaultTargetTriple(), *diags));
  driver->setCheckInputsExist(false);
  compilation.reset(driver->BuildCompilation(argv));
  DumpDiagnosticBuffer(diag_client);

  if (!compilation) {
    return false;
  }

  SharedPtr<opt::OptTable> opts(createDriverOptTable());
  bool result = false;
  const auto& job_list = compilation->getJobs();
  for (const auto& command : job_list) {
    // It's a kind of heuristics to skip non-Clang commands.
    // TODO: move this code inside |DriverCommand| constructor.
    if ((command.getSource().getKind() != Action::AssembleJobClass &&
         command.getSource().getKind() != Action::BackendJobClass &&
         command.getSource().getKind() != Action::PreprocessJobClass &&
         command.getSource().getKind() != Action::CompileJobClass) ||
        !std::regex_match(command.getExecutable(),
                          std::regex(".*/?clang(\\+\\+)?"))) {
      commands.emplace_back(new DriverCommand(command, compilation, driver));
      continue;
    }

    // TODO: we fallback to original Clang, if there is no Clang commands,
    //       since we have a problem with linking step:
    //       "no such file or directory: '@lib/libxxx.so.rsp'".
    //       We should fix this problem.
    result = true;

    auto* driver_command =
        new DriverCommand(command, opts, compilation, driver);

    if (diags->hasErrorOccurred()) {
      delete driver_command;
      return false;
    }

    commands.emplace_back(driver_command);
  }

  // This fake command won't be necessary, if someone solves the bug:
  // http://llvm.org/bugs/show_bug.cgi?id=20491
  const auto& temp_files = compilation->getTempFiles();
  if (!temp_files.empty()) {
    commands.emplace_back(new CleanCommand(temp_files));
  }

  DumpDiagnosticBuffer(diag_client);
  if (!result) {
    LOG(WARNING) << "No Clang commands found";
  }
  return result;
}

}  // namespace client
}  // namespace dist_clang
