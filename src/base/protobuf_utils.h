#pragma once

#include <base/aliases.h>
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

bool LoadFromFile(const String& path, google::protobuf::Message* message,
                  String* error = nullptr);
bool SaveToFile(const String& path, const google::protobuf::Message& message,
                String* error = nullptr);

}  // namespace base
}  // namespace dist_clang
