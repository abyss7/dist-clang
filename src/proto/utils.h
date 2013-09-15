#pragma once

#include <google/protobuf/text_format.h>
#include <iostream>
#include <string>

namespace dist_clang {
namespace proto {

inline void PrintMessage(const google::protobuf::Message& message) {
  std::string message_str;
  if (google::protobuf::TextFormat::PrintToString(message, &message_str))
    std::cout << message_str;
}

}  // namespace proto
}  // namespace dist_clang
