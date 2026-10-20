#pragma once

#include <base/types.h>
#include <base/logging.h>

namespace google {
namespace protobuf {

class Message;

}  // namespace protobuf
}  // namespace google

namespace dist_clang {
namespace base {

template <>
Log& Log::operator<<(const google::protobuf::Message& info);

// With |skip_unknown| the fields, that the message doesn't have, are skipped -
// for the files of the newer versions. Otherwise they are the errors - e.g. for
// the misprints in the configs.
bool LoadFromFile(const Path& path, google::protobuf::Message* message,
                  String* error = nullptr, bool skip_unknown = false);
// Sets only the fields from the file - the others stay. The message isn't
// changed, if the file can't be read or parsed.
bool MergeFromFile(const Path& path, google::protobuf::Message* message,
                   String* error = nullptr);
bool SaveToFile(const Path& path, const google::protobuf::Message& message,
                String* error = nullptr);

// The same in the binary format - faster, but not for people.
bool LoadBinaryFromFile(const Path& path, google::protobuf::Message* message,
                        String* error = nullptr);
bool SaveBinaryToFile(const Path& path,
                      const google::protobuf::Message& message,
                      String* error = nullptr);

}  // namespace base
}  // namespace dist_clang
