#include <base/protobuf_utils.h>

#include <base/const_string.h>
#include <base/file/file.h>
#include <third_party/protobuf/exported/src/google/protobuf/io/zero_copy_stream_impl.h>
#include <third_party/protobuf/exported/src/google/protobuf/text_format.h>

namespace dist_clang {
namespace base {

template <>
Log& Log::operator<<(const google::protobuf::Message& info) {
  String str;
  if (google::protobuf::TextFormat::PrintToString(info, &str)) {
    stream_ << str;
  }
  return *this;
}

bool LoadFromFile(const String& path, google::protobuf::Message* message) {
  File file(path);
  if (!file.IsValid()) {
    message->Clear();
    return false;
  }

  google::protobuf::io::FileInputStream input(file.native());
  input.SetCloseOnDelete(true);
  if (!google::protobuf::TextFormat::Parse(&input, message)) {
    message->Clear();
    return false;
  }

  return true;
}

bool SaveToFile(const String& path, const google::protobuf::Message& message) {
  String output;
  if (!google::protobuf::TextFormat::PrintToString(message, &output) ||
      !File::Write(path, Immutable(output))) {
    return false;
  }

  return true;
}

}  // namespace base
}  // namespace dist_clang
