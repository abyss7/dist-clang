#include <base/protobuf_utils.h>

#include <base/const_string.h>
#include <base/file/file.h>
#include <base/logging.h>
#include <third_party/protobuf/exported/src/google/protobuf/io/zero_copy_stream_impl.h>
#include <third_party/protobuf/exported/src/google/protobuf/text_format.h>

#include <base/using_log.h>

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

bool LoadFromFile(const String& path, google::protobuf::Message* message,
                  String* error) {
  Immutable contents;
  if (!File::Read(path, &contents, error) ||
      !google::protobuf::TextFormat::ParseFromString(contents, message)) {
    message->Clear();

    if (!contents.empty()) {
      LOG(VERBOSE) << "Protobuf file contents: " << std::endl << contents;
    }
    return false;
  }

  return true;
}

bool SaveToFile(const String& path, const google::protobuf::Message& message,
                String* error) {
  String output;
  if (!google::protobuf::TextFormat::PrintToString(message, &output) ||
      !File::Write(path, Immutable(output), error)) {
    return false;
  }

  return true;
}

}  // namespace base
}  // namespace dist_clang
