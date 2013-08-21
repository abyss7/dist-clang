#include "client/client_unix.h"

#include "base/c_utils.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using google::protobuf::Message;
using std::string;

UnixClient::UnixClient()
  : fd_(-1) {}
UnixClient::~UnixClient() {
  if (fd_ != -1)
    close(fd_);
}

bool UnixClient::Connect(const string& path, string* error) {
  sockaddr_un address;
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, path.c_str());

  fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
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

bool UnixClient::Receive(Message* message, string* error) {
  // TODO: implement this.
  error->assign("TODO: UnixSocket::Receive not implemented.");
  return false;
}

bool UnixClient::Send(const Message& message, string* /* error */) {
  return message.SerializeToFileDescriptor(fd_);
}
