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

bool LoadFromFile(const Path& path, google::protobuf::Message* message,
                  String* error = nullptr);
bool SaveToFile(const Path& path, const google::protobuf::Message& message,
                String* error = nullptr);

}  // namespace base
}  // namespace dist_clang
