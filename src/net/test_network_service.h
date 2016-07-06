#pragma once

#include <base/empty_lambda.h>
#include <net/network_service.h>

namespace std {

template <class T, class U>
struct hash<pair<T, U>> {
 public:
  size_t operator()(const pair<T, U>& value) const {
    size_t seed = 0;
    HashCombine(seed, value.first);
    HashCombine(seed, value.second);
    return seed;
  }

 private:
  template <class V, class Hash = std::hash<V>>
  inline void HashCombine(std::size_t& seed, const V& value) const {
    Hash hash;
    seed ^= hash(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  }
};

}  // namespace std

namespace dist_clang {
namespace net {

class TestNetworkService : public NetworkService {
 public:
  using OnConnectCallback = Fn<TestConnectionPtr(EndPointPtr, String*)>;
  using OnListenCallback = Fn<bool(const String&, ui16, String*)>;

  class Factory : public NetworkService::Factory {
   public:
    using OnCreateCallback = Fn<void(TestNetworkService*)>;

    UniquePtr<NetworkService> Create(ui32 read_timeout_secs,
                                     ui32 send_timeout_secs,
                                     ui32 read_min_bytes,
                                     ui32 connect_timeout_secs) override;

    inline void CallOnCreate(OnCreateCallback callback) {
      on_create_ = callback;
    }

   private:
    OnCreateCallback on_create_ = EmptyLambda<>();
  };

  inline bool Run() override { return true; }

  bool Listen(const String& path, ListenCallback callback,
              String* error) override;

  bool Listen(const String& host, ui16 port, bool ipv6, ListenCallback callback,
              String* error) override;

  virtual ConnectionPtr Connect(EndPointPtr end_point, String* error) override;

  ConnectionPtr TriggerListen(const String& host, ui16 port = 0);

  inline void CallOnConnect(OnConnectCallback callback) {
    on_connect_ = callback;
  }
  inline void CallOnListen(OnListenCallback callback) { on_listen_ = callback; }
  inline void CountConnectAttempts(Atomic<ui32>* counter) {
    connect_attempts_ = counter;
  }
  inline void CountListenAttempts(Atomic<ui32>* counter) {
    listen_attempts_ = counter;
  }

 private:
  using HostPortPair = Pair<String, ui16>;

  OnConnectCallback on_connect_ = EmptyLambda<TestConnectionPtr>();
  OnListenCallback on_listen_ = EmptyLambda<bool>(false);
  Atomic<ui32>* connect_attempts_ = nullptr;
  Atomic<ui32>* listen_attempts_ = nullptr;
  HashMap<HostPortPair, ListenCallback> listen_callbacks_;
};

}  // namespace net
}  // namespace dist_clang
