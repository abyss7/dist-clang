#pragma once

#include <base/file/data.h>
#include <net/connection_forward.h>

namespace dist_clang {
namespace net {

class Socket final : public base::Data {
 public:
  enum class ConnectionStatus {
    CONNECTED,
    CONNECTING,
    FAILED
  };

  explicit Socket(EndPointPtr peer);

  bool Bind(EndPointPtr peer, String* error = nullptr);
  bool Connect(EndPointPtr peer, String* error = nullptr);
  ConnectionStatus StartConnecting(EndPointPtr peer, String* error = nullptr);
  bool GetPendingError(String* error = nullptr);

  bool ReuseAddress(String* error = nullptr);
  bool SendTimeout(ui32 sec_timeout, String* error = nullptr);
  bool ReadTimeout(ui32 sec_timeout, String* error = nullptr);
  bool ReadLowWatermark(ui64 bytes_min, String* error = nullptr);

 private:
  friend class Passive;

  Socket() = default;
  explicit Socket(NativeType fd) : Data(fd) {}
};

}  // namespace net
}  // namespace dist_clang
