#include <client/driver_command.hh>

#include <base/assert.h>
#include <base/process_impl.h>

namespace dist_clang {
namespace client {

base::ProcessPtr DriverCommand::CreateProcess(Immutable current_dir,
                                              ui32 user_id) const {
  CHECK(!clang_);
  auto process =
      base::Process::Create(command_->getExecutable(), current_dir, user_id);
  for (const auto& it : command_->getArguments()) {
    process->AppendArg(Immutable(String(it)));
  }
  return process;
}

String DriverCommand::GetExecutable() const {
  return command_->getExecutable();
}

String DriverCommand::RenderAllArgs() const {
  String result;

  for (const auto& arg : command_->getArguments()) {
    result += String(" ") + arg;
  }

  return result.substr(1);
}

}  // namespace client
}  // namespace dist_clang
