#include "client/client_unix.h"

#include "base/c_utils.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <memory>

using google::protobuf::Message;
using std::string;
using namespace google::protobuf::io;

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

bool UnixClient::Receive(Message* message, string* /* error */) {
  std::unique_ptr<ZeroCopyInputStream> file_stream;
  std::unique_ptr<CodedInputStream> coded_stream;
  file_stream.reset(new FileInputStream(fd_));
  coded_stream.reset(new CodedInputStream(file_stream.get()));

  return message->ParseFromCodedStream(coded_stream.get());
}

bool UnixClient::Send(const Message& message, string* /* error */) {
  std::unique_ptr<ZeroCopyOutputStream> file_stream;
  std::unique_ptr<CodedOutputStream> coded_stream;
  file_stream.reset(new FileOutputStream(fd_));
  coded_stream.reset(new CodedOutputStream(file_stream.get()));

  return message.SerializeToCodedStream(coded_stream.get());
}
