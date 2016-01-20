#include <client/command.h>

#include <base/assert.h>
#include <base/base.pb.h>
#include <base/c_utils.h>
#include <base/logging.h>
#include <base/process_impl.h>
#include <base/string_utils.h>

#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/VirtualFileSystem.h>
#include <clang/Driver/Action.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/DriverDiagnostic.h>
#include <clang/Driver/Options.h>
#include <clang/Driver/ToolChain.h>
#include <clang/Frontend/TextDiagnosticBuffer.h>
#include <llvm/Option/Arg.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

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

DriverCommand* WEAK_PTR Command::AsDriverCommand() {
  DCHECK(IsClang());
  return static_cast<DriverCommand*>(this);
}

// static
bool DriverCommand::GenerateFromArgs(int argc, const char* const raw_argv[],
                                     List& commands) {
  using namespace clang;
  using namespace clang::driver;
  using namespace llvm;

  SmallVector<const char*, 256> argv;
  SpecificBumpPtrAllocator<char> arg_allocator;
  auto&& arg_array = ArrayRef<const char*>(raw_argv, argc);
  llvm::sys::Process::GetArgumentVector(argv, arg_array, arg_allocator);

  llvm::InitializeAllTargets();  // Multiple calls per program are allowed.

  IntrusiveRefCntPtr<DiagnosticOptions> diag_opts = new DiagnosticOptions;
  TextDiagnosticBuffer* diag_client = new TextDiagnosticBuffer;
  IntrusiveRefCntPtr<DiagnosticIDs> diag_id(new DiagnosticIDs());
  IntrusiveRefCntPtr<clang::DiagnosticsEngine> diags;
  SharedPtr<Compilation> compilation;
  SharedPtr<Driver> driver;

  diags = new DiagnosticsEngine(diag_id, &*diag_opts, diag_client);

  String path = argv[0];
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
    if ((command.getSource().getKind() != Action::AssembleJobClass &&
         command.getSource().getKind() != Action::BackendJobClass &&
         command.getSource().getKind() != Action::CompileJobClass &&
         command.getSource().getKind() != Action::PrecompileJobClass &&
         command.getSource().getKind() != Action::PreprocessJobClass) ||
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

    const unsigned included_flags_bitmask = options::CC1Option;
    unsigned missing_arg_index, missing_arg_count;
    auto* driver_command = new DriverCommand(
        opts->ParseArgs(command.getArguments(), missing_arg_index,
                        missing_arg_count, included_flags_bitmask),
        compilation, opts, driver);

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

void DriverCommand::FillFlags(base::proto::Flags* flags,
                              const String& clang_path,
                              const String& clang_major_version) const {
  DCHECK(IsClang());

  if (!flags) {
    return;
  }

  flags->Clear();

  llvm::opt::ArgStringList non_direct_list, non_cached_list, other_list;

  for (const auto& arg : arg_list_) {
    using namespace clang::driver::options;

    // TODO: try to sort out flags by some attribute, i.e. group actions,
    //       compilation-only flags, etc.

    if (arg->getOption().getKind() == llvm::opt::Option::InputClass) {
      flags->set_input(arg->getValue());
    } else if (arg->getOption().matches(OPT_add_plugin)) {
      arg->render(arg_list_, other_list);
      flags->mutable_compiler()->add_plugins()->set_name(arg->getValue());
    } else if (arg->getOption().matches(OPT_emit_obj)) {
      flags->set_action(arg->getSpelling());
    } else if (arg->getOption().matches(OPT_E)) {
      flags->set_action(arg->getSpelling());
    } else if (arg->getOption().matches(OPT_S)) {
      flags->set_action(arg->getSpelling());
    } else if (arg->getOption().matches(OPT_fsyntax_only)) {
      flags->set_action(arg->getSpelling());
    } else if (arg->getOption().matches(OPT_dependency_file)) {
      flags->set_deps_file(arg->getValue());
    } else if (arg->getOption().matches(OPT_load)) {
      // FIXME: maybe claim this type of args right after generation?
    } else if (arg->getOption().matches(OPT_mrelax_all)) {
      flags->add_cc_only(arg->getSpelling());
    } else if (arg->getOption().matches(OPT_o)) {
      flags->set_output(arg->getValue());
    } else if (arg->getOption().matches(OPT_x)) {
      flags->set_language(arg->getValue());
    }

    // Non-cacheable flags.
    // NOTICE: we should be very cautious here, since the local compilations
    //         are performed on a non-preprocessed file, but the result is
    //         saved using the hash from a preprocessed file.
    else if (arg->getOption().matches(OPT_include) ||
             arg->getOption().matches(OPT_internal_externc_isystem) ||
             arg->getOption().matches(OPT_isysroot) ||
             arg->getOption().matches(OPT_D) ||
             arg->getOption().matches(OPT_I)) {
      arg->render(arg_list_, non_cached_list);
    } else if (arg->getOption().matches(OPT_coverage_file) ||
               arg->getOption().matches(OPT_fdebug_compilation_dir) ||
               arg->getOption().matches(OPT_ferror_limit) ||
               arg->getOption().matches(OPT_main_file_name) ||
               arg->getOption().matches(OPT_MF) ||
               arg->getOption().matches(OPT_MMD) ||
               arg->getOption().matches(OPT_MT)) {
      arg->render(arg_list_, non_direct_list);
    } else if (arg->getOption().matches(OPT_internal_isystem) ||
               arg->getOption().matches(OPT_resource_dir)) {
      // Use --internal-isystem and --resource_dir based on real Clang path.
      non_cached_list.push_back(arg->getSpelling().data());

      String replaced_command = arg->getValue();
      if (replaced_command[0] != '/') {
        replaced_command = base::GetSelfPath() + '/' + replaced_command;
      }

      std::regex path_regex("(" + base::EscapeRegex(base::GetSelfPath()) + ")");
      replaced_command = std::regex_replace(
          replaced_command, path_regex,
          clang_path.substr(0, clang_path.find_last_of('/')));

      std::regex version_regex("(\\/lib\\/clang\\/\\d+\\.\\d+\\.\\d+)");
      // FIXME: Clang internally hardcodes path according to its major version.
      replaced_command = std::regex_replace(
          replaced_command, version_regex, "/lib/clang/" + clang_major_version);

      non_cached_list.push_back(arg_list_.MakeArgString(replaced_command));
      LOG(VERBOSE) << "Replaced command: " << non_cached_list.back();
    }

    // By default all other flags are cacheable.
    else {
      arg->render(arg_list_, other_list);
    }
  }

  for (const auto& value : non_direct_list) {
    flags->add_non_direct(value);
  }
  for (const auto& value : non_cached_list) {
    flags->add_non_cached(value);
  }
  for (const auto& value : other_list) {
    flags->add_other(value);
  }
}

base::ProcessPtr DriverCommand::CreateProcess(Immutable current_dir,
                                              ui32 user_id) const {
  CHECK(command_);
  auto process =
      base::Process::Create(command_->getExecutable(), current_dir, user_id);
  for (const auto& it : command_->getArguments()) {
    process->AppendArg(Immutable(String(it)));
  }
  return process;
}

String DriverCommand::GetExecutable() const {
  if (!IsClang()) {
    return command_->getExecutable();
  } else {
    return base::GetSelfPath() + "/clang";
  }
}

String DriverCommand::RenderAllArgs() const {
  String result;

  if (IsClang()) {
    for (const auto& arg : arg_list_) {
      result += String(" ") + arg->getAsString(arg_list_);
    }
  } else {
    for (const auto& arg : command_->getArguments()) {
      result += String(" ") + arg;
    }
  }

  return result.substr(1);
}

base::ProcessPtr CleanCommand::CreateProcess(Immutable current_dir,
                                             ui32 user_id) const {
  auto process = base::Process::Create(rm_path, current_dir, user_id);
  for (const auto& it : temp_files_) {
    process->AppendArg(Immutable(String(it)));
  }
  return process;
}

String CleanCommand::RenderAllArgs() const {
  String result;

  for (const auto& arg : temp_files_) {
    result += String(" ") + arg;
  }

  return result.substr(1);
}

}  // namespace client
}  // namespace dist_clang
