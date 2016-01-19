#include <client/clean_command.hh>

#include <base/process_impl.h>

namespace dist_clang {
namespace client {

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
