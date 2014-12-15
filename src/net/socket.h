#pragma once

#include <base/file/data.h>
#include <net/connection_forward.h>

namespace dist_clang {
namespace net {

class Socket final : public base::Data {
 public:
  Socket() = default;
  explicit Socket(NativeType fd) : Data(fd) {}
  explicit Socket(EndPointPtr peer);

  bool Bind(EndPointPtr peer, String* error = nullptr);
  bool Connect(EndPointPtr peer, String* error = nullptr);

  bool ReuseAddress(String* error = nullptr);
  bool SendTimeout(ui32 sec_timeout, String* error = nullptr);
  bool ReadTimeout(ui32 sec_timeout, String* error = nullptr);
  bool ReadLowWatermark(ui64 bytes_min, String* error = nullptr);
};

}  // namespace net
}  // namespace dist_clang
