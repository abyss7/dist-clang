#include "net/socket_output_stream.h"

#include "base/assert.h"
#include "net/base/end_point.h"

#include <sys/socket.h>
#include <unistd.h>

namespace {

int close_no_eintr(int fd) {
  int result;
  do {
    result = close(fd);
  }
  while (result < 0 && errno == EINTR);
  return result;
}

}  // namespace

namespace dist_clang {
namespace net {

SocketOutputStream::SocketOutputStream(fd_t fd, EndPointPtr end_point,
                                       int block_size)
  : copying_output_(fd, end_point), impl_(&copying_output_, block_size) {
}

SocketOutputStream::~SocketOutputStream() {
  impl_.Flush();
}

bool SocketOutputStream::Close() {
  bool flush_succeeded = impl_.Flush();
  return copying_output_.Close() && flush_succeeded;
}

bool SocketOutputStream::Flush() {
  return impl_.Flush();
}

bool SocketOutputStream::Next(void** data, int* size) {
  return impl_.Next(data, size);
}

void SocketOutputStream::BackUp(int count) {
  impl_.BackUp(count);
}

int64_t SocketOutputStream::ByteCount() const {
  return impl_.ByteCount();
}

SocketOutputStream::CopyingStream::CopyingStream(fd_t fd, EndPointPtr end_point)
  : file_(fd), end_point_(end_point), close_on_delete_(false),
    is_closed_(false), errno_(0) {
}

SocketOutputStream::CopyingStream::~CopyingStream() {
  if (close_on_delete_) {
    if (!Close()) {
      std::cerr << "close() failed: " << strerror(errno_) << std::endl;
    }
  }
}

bool SocketOutputStream::CopyingStream::Close() {
  CHECK(!is_closed_);

  is_closed_ = true;
  if (close_no_eintr(file_) != 0) {
    // The docs on close() do not specify whether a file descriptor is still
    // open after close() fails with EIO.  However, the glibc source code
    // seems to indicate that it is not.
    errno_ = errno;
    return false;
  }

  return true;
}

bool SocketOutputStream::CopyingStream::Write(const void* buffer, int size) {
  CHECK(!is_closed_);
  int total_written = 0;

  const uint8_t* buffer_base = reinterpret_cast<const uint8_t*>(buffer);

  while (total_written < size) {
    int bytes;
    do {
      if (end_point_) {
        bytes = sendto(file_, buffer_base + total_written, size - total_written,
                       MSG_FASTOPEN, *end_point_, end_point_->size());
      }
      else {
        bytes = write(file_, buffer_base + total_written, size - total_written);
      }
    }
    while (bytes < 0 && errno == EINTR);

    if (bytes <= 0) {
      if (bytes < 0) {
        errno_ = errno;
        std::cerr << strerror(errno_) << std::endl;
      }
      return false;
    }
    total_written += bytes;
  }

  return true;
}

}  // namespace net
}  // namespace dist_clang
