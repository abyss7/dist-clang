#pragma once

#include "net/base/types.h"
#include "net/connection_forward.h"
#include "third_party/protobuf/google/protobuf/io/zero_copy_stream.h"
#include "third_party/protobuf/google/protobuf/io/zero_copy_stream_impl_lite.h"

namespace dist_clang {
namespace net {

// Forked from google::protobuf::io::FileOutputStream.
class SocketOutputStream : public google::protobuf::io::ZeroCopyOutputStream {
  public:
    SocketOutputStream(fd_t fd, EndPointPtr end_point, int block_size = -1);
    SocketOutputStream(const SocketOutputStream&) = delete;
    void operator=(const SocketOutputStream&) = delete;
    ~SocketOutputStream();

    bool Close();
    bool Flush();
    inline void SetCloseOnDelete(bool value);
    inline int GetErrno();

    virtual bool Next(void** data, int* size) override;
    virtual void BackUp(int count) override;
    virtual int64_t ByteCount() const override;

  private:
    using CopyingOutputStream = google::protobuf::io::CopyingOutputStream;
    using CopyingOutputStreamAdaptor =
        google::protobuf::io::CopyingOutputStreamAdaptor;

    class CopyingStream : public CopyingOutputStream {
      public:
        CopyingStream(fd_t fd, EndPointPtr end_point);
        CopyingStream(const CopyingStream&) = delete;
        void operator=(const CopyingStream&) = delete;
        ~CopyingStream();

        bool Close();
        inline void SetCloseOnDelete(bool value);
        inline int GetErrno();

        virtual bool Write(const void* buffer, int size) override;

      private:
        const fd_t file_;
        EndPointPtr end_point_;
        bool close_on_delete_;
        bool is_closed_;

        // The errno of the I/O error, if one has occurred.  Otherwise, zero.
        int errno_;
    };

    CopyingStream copying_output_;
    CopyingOutputStreamAdaptor impl_;
};

void SocketOutputStream::SetCloseOnDelete(bool value) {
  copying_output_.SetCloseOnDelete(value);
}

int SocketOutputStream::GetErrno() {
  return copying_output_.GetErrno();
}

void SocketOutputStream::CopyingStream::SetCloseOnDelete(bool value) {
  close_on_delete_ = value;
}

int SocketOutputStream::CopyingStream::GetErrno() {
  return errno_;
}

}  // namespace net
}  // namespace dist_clang
