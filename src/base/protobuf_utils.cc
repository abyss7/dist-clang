#include <base/protobuf_utils.h>

#include <base/const_string.h>
#include <base/file/file.h>
#include <base/logging.h>
#include <google/protobuf/io/tokenizer.h>
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

namespace {

// Keeps the first error of the parsing - with its place in the file.
class FirstError : public google::protobuf::io::ErrorCollector {
 public:
  void AddError(int line, google::protobuf::io::ColumnNumber column,
                const std::string& message) override {
    if (error.empty()) {
      error = "line " + std::to_string(line + 1) + ", column " +
              std::to_string(column + 1) + ": " + message;
    }
  }

  String error;
};

bool Parse(const String& contents, google::protobuf::Message* message,
           bool merge, bool skip_unknown, String* error) {
  FirstError collector;
  google::protobuf::TextFormat::Parser parser;
  parser.RecordErrorsTo(&collector);
  parser.AllowUnknownField(skip_unknown);
  if (merge ? parser.MergeFromString(contents, message)
            : parser.ParseFromString(contents, message)) {
    return true;
  }
  if (error) {
    error->assign(collector.error.empty() ? String("Can't parse the file")
                                          : collector.error);
  }
  return false;
}

}  // namespace

bool LoadFromFile(const Path& path, google::protobuf::Message* message,
                  String* error, bool skip_unknown) {
  Immutable contents;
  if (!File::Read(path, &contents, error) ||
      !Parse(contents.string_copy(), message, false, skip_unknown, error)) {
    message->Clear();

    if (!contents.empty()) {
      LOG(VERBOSE) << "Protobuf file contents: " << std::endl << contents;
    }
    return false;
  }

  return true;
}

bool MergeFromFile(const Path& path, google::protobuf::Message* message,
                   String* error) {
  Immutable contents;
  if (!File::Read(path, &contents, error)) {
    return false;
  }
  UniquePtr<google::protobuf::Message> merged(message->New());
  merged->CopyFrom(*message);
  if (!Parse(contents.string_copy(), merged.get(), true, false, error)) {
    return false;
  }
  message->CopyFrom(*merged);
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
