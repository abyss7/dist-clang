#pragma once

#include <string>

class TCPClient {
  public:
    TCPClient();
    ~TCPClient();

    bool Connect(const std::string& host, uint16_t port, std::string* error);

  private:
    int fd_;
};
