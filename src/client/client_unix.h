#pragma once

#include <google/protobuf/message.h>
#include <string>

class UnixClient {
  public:
    UnixClient();
    ~UnixClient();

    bool Connect(const std::string& path, std::string* error);
    bool Receive(google::protobuf::Message* message, std::string* error);
    bool Send(const google::protobuf::Message& message, std::string* error);

  private:
    int fd_;
};
