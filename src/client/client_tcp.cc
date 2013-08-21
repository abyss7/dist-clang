#include "client/client_tcp.h"

#include "base/c_utils.h"

#include <netdb.h>

using std::string;

TCPClient::TCPClient()
  : fd_(-1) {}
TCPClient::~TCPClient() {
  if (fd_ != -1)
    close(fd_);
}

bool TCPClient::Connect(const string& host, uint16_t port, string* error) {
  struct hostent* host_entry;
  struct in_addr** address_list;

  if ((host_entry = gethostbyname(host.c_str())) == NULL) {
    GetLastError(error);
    return false;
  }

  address_list =
      reinterpret_cast<struct in_addr**>(host_entry->h_addr_list);

  sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = address_list[0]->s_addr;
  address.sin_port = htons(port);

  fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (fd_ == -1) {
    GetLastError(error);
    return false;
  }
  if (connect(fd_, reinterpret_cast<sockaddr*>(&address),
              sizeof(address)) == -1) {
    GetLastError(error);
    return false;
  }

  return true;
}
