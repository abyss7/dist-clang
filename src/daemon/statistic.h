#pragma once

#include "proto/stats.pb.h"

namespace dist_clang {

namespace net {
class NetworkServiceImpl;
}

namespace proto {
class Host;
}

namespace daemon {

class Statistic {
  public:
    using Metric = proto::Statistic::Metric;

    static void Initialize(net::NetworkServiceImpl& network_service,
                           const proto::Host& host);
    static void Accumulate(Metric metric, uint64_t value, uint64_t count = 1);
};

}  // namespace daemon
}  // namespace dist_clang
