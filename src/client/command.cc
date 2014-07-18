#include <client/command.h>

#include <base/assert.h>
#include <base/c_utils.h>
#include <base/logging.h>
#include <base/process_impl.h>
#include <proto/remote.pb.h>

#include <third_party/libcxx/exported/include/regex>

#include <clang/Basic/Diagnostic.h>
#include <clang/Driver/Action.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/DriverDiagnostic.h>
#include <clang/Driver/Options.h>
#include <clang/Frontend/TextDiagnosticBuffer.h>
#include <llvm/Option/Arg.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <base/using_log.h>

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

namespace dist_clang {
namespace client {

// static
bool Command::GenerateFromArgs(int argc, const char* const raw_argv[],
                               List<Command>& commands) {
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
  SharedPtr<clang::driver::Compilation> compilation;
  SharedPtr<Driver> driver;

  diags = new DiagnosticsEngine(diag_id, &*diag_opts, diag_client);

  std::string path = argv[0];
  driver.reset(new Driver(path, llvm::sys::getDefaultTargetTriple(), *diags));
  compilation.reset(driver->BuildCompilation(argv));
  DumpDiagnosticBuffer(diag_client);

  if (!compilation) {
    return false;
  }

  SharedPtr<opt::OptTable> opts(createDriverOptTable());
  bool result = false;
  const auto& jobs = compilation->getJobs();
  for (auto& job : jobs) {
    if (job->getKind() == Job::CommandClass) {
      auto command = static_cast<clang::driver::Command*>(job);

      // It's a kind of heuristics to skip non-Clang commands.
      if ((command->getSource().getKind() != Action::AssembleJobClass &&
           command->getSource().getKind() != Action::CompileJobClass &&
           command->getSource().getKind() != Action::PrecompileJobClass &&
           command->getSource().getKind() != Action::PreprocessJobClass) ||
          !std::regex_match(command->getExecutable(),
                            std::regex(".*/?clang(\\+\\+)?"))) {
        commands.emplace_back(std::move(Command(command, compilation, driver)));
        continue;
      }

      // TODO: we fallback to original Clang, if there is no Clang commands,
      //       since we have a problem with linking step:
      //       "no such file or directory: '@lib/libxxx.so.rsp'".
      //       We should fix this problem.
      result = true;

      auto arg_begin = command->getArguments().begin();
      auto arg_end = command->getArguments().end();

      const unsigned included_flags_bitmask = options::CC1Option;
      unsigned missing_arg_index, missing_arg_count;
      commands.emplace_back(std::move(
          Command(opts->ParseArgs(arg_begin, arg_end, missing_arg_index,
                                  missing_arg_count, included_flags_bitmask),
                  compilation, opts, driver)));
      const auto& arg_list = commands.back().arg_list_;

      // Check for missing argument error.
      if (missing_arg_count) {
        diags->Report(diag::err_drv_missing_argument)
            << arg_list->getArgString(missing_arg_index) << missing_arg_count;
        DumpDiagnosticBuffer(diag_client);
        return false;
      }

      // Issue errors on unknown arguments.
      for (auto it = arg_list->filtered_begin(options::OPT_UNKNOWN),
                end = arg_list->filtered_end();
           it != end; ++it) {
        diags->Report(diag::err_drv_unknown_argument)
            << (*it)->getAsString(*arg_list);
        DumpDiagnosticBuffer(diag_client);
        return false;
      }
    }
  }

  DumpDiagnosticBuffer(diag_client);
  if (!result) {
    LOG(WARNING) << "No Clang commands found";
  }
  return result;
}

void Command::FillFlags(proto::Flags* flags, const String& clang_path) const {
  CHECK(arg_list_);

  if (!flags) {
    return;
  }

  flags->Clear();

  llvm::opt::ArgStringList non_direct_list, non_cached_list, other_list;

  for (const auto& arg : *arg_list_) {
    using namespace clang::driver::options;

    if (arg->getOption().getKind() == llvm::opt::Option::InputClass) {
      flags->set_input(arg->getValue());
    } else if (arg->getOption().matches(OPT_add_plugin)) {
      arg->render(*arg_list_, other_list);
      flags->mutable_compiler()->add_plugins()->set_name(arg->getValue());
    } else if (arg->getOption().matches(OPT_emit_obj)) {
      flags->set_action(arg->getSpelling());
    } else if (arg->getOption().matches(OPT_E)) {
      flags->set_action(arg->getSpelling());
    } else if (arg->getOption().matches(OPT_dependency_file)) {
      flags->set_deps_file(arg->getValue());
    } else if (arg->getOption().matches(OPT_load)) {
      // FIXME: maybe claim this type of args right after generation?
      continue;
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
             arg->getOption().matches(OPT_resource_dir) ||
             arg->getOption().matches(OPT_D) ||
             arg->getOption().matches(OPT_I)) {
      arg->render(*arg_list_, non_cached_list);
    } else if (arg->getOption().matches(OPT_coverage_file) ||
               arg->getOption().matches(OPT_fdebug_compilation_dir) ||
               arg->getOption().matches(OPT_ferror_limit) ||
               arg->getOption().matches(OPT_main_file_name) ||
               arg->getOption().matches(OPT_MF) ||
               arg->getOption().matches(OPT_MMD) ||
               arg->getOption().matches(OPT_MT)) {
      arg->render(*arg_list_, non_direct_list);
    } else if (arg->getOption().matches(OPT_internal_isystem)) {
      // Use --internal-isystem based on real clang path.
      auto EscapeRegex = [](const String& str) {
        const std::regex regex(R"([\)\{\}\[\]\(\)\^\$\.\|\*\+\?\\])");
        const String replace(R"(\$&)");
        return std::regex_replace(str, regex, replace);
      };

      std::regex regex("(" + EscapeRegex(base::GetSelfPath()) + ")");
      non_cached_list.push_back(arg->getSpelling().data());
      non_cached_list.push_back(arg_list_->MakeArgString(std::regex_replace(
          arg->getValue(), regex,
          clang_path.substr(0, clang_path.find_last_of('/')))));
    }

    // By default all other flags are cacheable.
    else {
      arg->render(*arg_list_, other_list);
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

base::ProcessPtr Command::CreateProcess(const String& current_dir,
                                        ui32 user_id) const {
  CHECK(command_);
  auto process =
      base::Process::Create(command_->getExecutable(), current_dir, user_id);
  process->AppendArg(command_->getArguments().begin(),
                     command_->getArguments().end());
  return process;
}

String Command::GetExecutable() const {
  if (command_) {
    return command_->getExecutable();
  } else if (arg_list_) {
    return base::GetSelfPath() + "/clang";
  } else {
    NOTREACHED();
    return String();
  }
}

String Command::RenderAllArgs() const {
  String result;

  if (arg_list_) {
    for (const auto& arg : *arg_list_) {
      result += String(" ") + arg->getAsString(*arg_list_);
    }
  } else if (command_) {
    for (const auto& arg : command_->getArguments()) {
      result += String(" ") + arg;
    }
  } else {
    NOTREACHED();
  }

  return result.substr(1);
}

}  // namespace client
}  // namespace dist_clang
