#include <daemon/collector.h>

#include <base/assert.h>
#include <base/logging.h>
#include <net/connection_impl.h>
#include <perf/stat_service.h>

#include <base/using_log.h>

namespace dist_clang {
namespace daemon {

Collector::Collector(const proto::Configuration& configuration)
    : BaseDaemon(configuration) {
  CHECK(configuration.has_collector());
}

bool Collector::Initialize() {
  String error;
  const auto& local = conf()->collector().local();
  if (!Listen(local.host(), local.port(), local.ipv6(), &error)) {
    LOG(ERROR) << "Collector failed to listen on " << local.host() << ":"
               << local.port() << " : " << error;
    return false;
  }

  return BaseDaemon::Initialize();
}

bool Collector::HandleNewMessage(net::ConnectionPtr connection,
                                 Universal message,
                                 const net::proto::Status& status) {
  if (!message->IsInitialized()) {
    LOG(INFO) << message->InitializationErrorString();
    return false;
  }

  if (status.code() != net::proto::Status::OK) {
    LOG(ERROR) << status.description();
    return connection->ReportStatus(status);
  }

  if (message->HasExtension(perf::proto::Report::extension)) {
    UniquePtr<perf::proto::Report> report(
        message->ReleaseExtension(perf::proto::Report::extension));
    for (auto& metric : *report->mutable_metric()) {
      if (metric.has_name()) {
        base::Singleton<perf::StatService>::Get().Dump(metric);
      }
    }
    if (!connection->SendSync(std::move(report))) {
      LOG(WARNING) << "Failed to send report message!";
    }

    return true;
  }

  NOTREACHED();
  return false;
}

}  // namespace daemon
}  // namespace dist_clang
