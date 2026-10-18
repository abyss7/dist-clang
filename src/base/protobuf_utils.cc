#include <base/protobuf_utils.h>

#include <base/const_string.h>
#include <base/file/file.h>
#include <base/logging.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>

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

bool LoadFromFile(const Path& path, google::protobuf::Message* message,
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

bool SaveToFile(const Path& path, const google::protobuf::Message& message,
                String* error) {
  String output;
  if (!google::protobuf::TextFormat::PrintToString(message, &output) ||
      !File::Write(path, Immutable(output), error)) {
    return false;
  }

  return true;
}

bool LoadBinaryFromFile(const Path& path, google::protobuf::Message* message,
                        String* error) {
  Immutable contents;
  if (!File::Read(path, &contents, error) ||
      !message->ParseFromArray(contents.data(), contents.size())) {
    message->Clear();
    return false;
  }
  return true;
}

bool SaveBinaryToFile(const Path& path,
                      const google::protobuf::Message& message,
                      String* error) {
  String output;
  return message.SerializeToString(&output) &&
         File::Write(path, Immutable(std::move(output)), error);
}

}  // namespace base
}  // namespace dist_clang
