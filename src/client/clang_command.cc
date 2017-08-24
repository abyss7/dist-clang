#include <client/clang_command.hh>

#include <base/assert.h>
#include <base/base.pb.h>
#include <base/c_utils.h>
#include <base/logging.h>
#include <base/process_impl.h>

#include <clang/Driver/Options.h>

#include <base/using_log.h>

namespace {

// Flags to be ignored that are known to somehow break compilation.
const std::array<clang::driver::options::ID, 2> kIgnoredFlags = {{
  clang::driver::options::OPT_frewrite_includes,
  clang::driver::options::OPT_rewrite_macros,
}};

}  // namespace

namespace dist_clang {
namespace client {

ClangCommand::ClangCommand(llvm::ArrayRef<const char*> args,
                           SharedPtr<llvm::opt::OptTable> opts)
    : arg_list_([&] {
        // In the first place we need to convert driver arguments to cc1 options
        // - to provide the cross-compilation feature.
        const unsigned included_flags_bitmask =
            clang::driver::options::CC1Option;
        unsigned missing_arg_index, missing_arg_count;
        return opts->ParseArgs(args, missing_arg_index, missing_arg_count,
                               included_flags_bitmask);
      }()),
      opts_(opts) {}

base::ProcessPtr ClangCommand::CreateProcess(Immutable current_dir,
                                             ui32 user_id) const {
  NOTREACHED();
  return base::ProcessPtr();
}

String ClangCommand::GetExecutable() const {
  NOTREACHED();
  return String();
}

String ClangCommand::RenderAllArgs() const {
  String result;
  for (const auto& arg : arg_list_) {
    result += String(" ") + arg->getAsString(arg_list_);
  }
  return result.substr(1);
}

bool ClangCommand::FillFlags(base::proto::Flags* flags,
                             const String& clang_path,
                             const String& clang_major_version,
                             bool rewrite_includes) const {
  using namespace clang::driver::options;
  if (!flags) {
    return true;
  }

  flags->Clear();

  llvm::opt::ArgStringList non_direct_list, non_cached_list, other_list;
  llvm::opt::DerivedArgList tmp_list(arg_list_);

  for (const auto& arg : arg_list_) {

    // TODO: try to sort out flags by some attribute, i.e. group actions,
    //       compilation-only flags, etc.

    if (arg->getOption().getKind() == llvm::opt::Option::InputClass) {
      flags->set_input(arg->getValue());
    } else if (arg->getOption().matches(OPT_add_plugin)) {
      arg->render(arg_list_, other_list);
      flags->mutable_compiler()->add_plugins()->set_name(arg->getValue());
    } else if (arg->getOption().matches(OPT_emit_obj) ||
               arg->getOption().matches(OPT_E) ||
               arg->getOption().matches(OPT_S) ||
               arg->getOption().matches(OPT_fsyntax_only)) {
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
    } else if (arg->getOption().matches(OPT_fsanitize_blacklist)) {
      flags->set_sanitize_blacklist(arg->getValue());
    }

    // Non-cacheable flags.
    // NOTICE: we should be very cautious here, since the local compilations
    //         are performed on a non-preprocessed file, but the result is
    //         saved using the hash from a preprocessed file.
    else if (arg->getOption().matches(OPT_include) ||
             arg->getOption().matches(OPT_internal_externc_isystem) ||
             arg->getOption().matches(OPT_isysroot) ||
             arg->getOption().matches(OPT_I)) {
      arg->render(arg_list_, non_cached_list);
    } else if (arg->getOption().matches(OPT_D)) {
      if (rewrite_includes) {
        arg->render(arg_list_, other_list);
      } else {
        arg->render(arg_list_, non_cached_list);
      }
    } else if (arg->getOption().matches(OPT_include_pch) ||
               arg->getOption().matches(OPT_include_pth)) {
      flags->add_included_files(arg->getValue());

      // FIXME: don't render arguments here - render them somewhere in
      //        |CompilationDaemon|, but for now we don't pass the argument
      //        type.
      arg->render(arg_list_, non_cached_list);
    } else if (arg->getOption().matches(OPT_coverage_data_file) ||
               arg->getOption().matches(OPT_coverage_notes_file) ||
               arg->getOption().matches(OPT_fdebug_compilation_dir) ||
               arg->getOption().matches(OPT_ferror_limit) ||
               arg->getOption().matches(OPT_main_file_name) ||
               arg->getOption().matches(OPT_MF) ||
               arg->getOption().matches(OPT_MMD) ||
               arg->getOption().matches(OPT_MT)) {
      arg->render(arg_list_, non_direct_list);
    } else if (arg->getOption().matches(OPT_internal_isystem) ||
               arg->getOption().matches(OPT_resource_dir)) {
      // Relative -internal-isystem and -resource_dir are based on current
      // working directory - not on a Clang installation directory.

      String replaced_command = arg->getValue();

      // FIXME: It's a hack. Clang internally hardcodes path according to its
      //        major version.
      // TODO: use InstallDir instead of hardcoded regex.
      std::regex version_regex("(\\/lib\\/clang\\/\\d+\\.\\d+\\.\\d+)");
      replaced_command = std::regex_replace(
          replaced_command, version_regex, "/lib/clang/" + clang_major_version);

      String self_path;
      String error;
      if (!base::GetSelfPath(self_path, &error)) {
        LOG(WARNING) << "Failed to get executable path: " << error;
        return false;
      }

      // Assume the -resource-dir and -internal-isystem that are based on Clang
      // installation path to be the same for all compilers with the same
      // version - no need to use them in direct cache.
      auto pos = replaced_command.find(self_path);
      if (pos != String::npos) {
        replaced_command.replace(
            pos, self_path.size(),
            clang_path.substr(0, clang_path.find_last_of('/')));
        non_direct_list.push_back(arg->getSpelling().data());
        non_direct_list.push_back(tmp_list.MakeArgString(replaced_command));
        LOG(VERBOSE) << "Replaced command: " << non_direct_list.back();
      } else {
        non_cached_list.push_back(arg->getSpelling().data());
        non_cached_list.push_back(tmp_list.MakeArgString(replaced_command));
        LOG(VERBOSE) << "Replaced command: " << non_cached_list.back();
      }
    }

    // By default all other flags are cacheable.
    else {
      // FIXME: Potentially this is an O(n*m) problem, that should be solved in
      //        a more efficient way.
      const bool ignored = std::any_of(kIgnoredFlags.begin(),
                                       kIgnoredFlags.end(),
                                       [&](const auto& ignored_flag) {
        return arg->getOption().matches(ignored_flag);
      });
      if (!ignored) {
        arg->render(arg_list_, other_list);
      }
    }
  }

  if (rewrite_includes) {
    const auto& option = opts_->getOption(OPT_frewrite_includes);
    String option_name = tmp_list.MakeArgString(option.getRenderName());
    option_name.insert(0, 1, '-');
    other_list.push_back(option_name.c_str());
  }
  flags->set_rewrite_includes(rewrite_includes);

  for (const auto& value : non_direct_list) {
    flags->add_non_direct(value);
  }
  for (const auto& value : non_cached_list) {
    flags->add_non_cached(value);
  }
  for (const auto& value : other_list) {
    flags->add_other(value);
  }

  return true;
}

}  // namespace client
}  // namespace dist_clang
