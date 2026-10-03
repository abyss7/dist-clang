#include <client/clang.h>

#include <base/base.pb.h>
#include <base/file_utils.h>
#include <base/logging.h>
#include <client/configuration.pb.h>
#include <net/connection.h>
#include <net/end_point.h>
#include <net/network_service_impl.h>
#include <perf/stat.pb.h>

#include STL(iomanip)
#include STL(iostream)

#include <unistd.h>

#include <base/using_log.h>

extern char** environ;

namespace dist_clang {
namespace client {

namespace {

net::ConnectionPtr Connect(const String& socket_path,
                           const proto::Configuration& config,
                           UniquePtr<net::NetworkService>* service) {
  *service = net::NetworkService::Create(
      config.connect_timeout(), config.read_timeout(), config.send_timeout(),
      config.read_minimum());

  String error;
  auto connection =
      (*service)->Connect(net::EndPoint::UnixSocket(socket_path), &error);
  if (!connection) {
    LOG(VERBOSE) << "Failed to connect to daemon: " << error;
  }
  return connection;
}

}  // namespace

Result DoMain(const List<String>& args, const String& socket_path,
              const proto::Configuration& config) {
  DCHECK(!args.empty());

  String error;
  auto current_dir = base::GetCurrentDir(&error);
  if (current_dir.empty()) {
    LOG(WARNING) << "Can't get current directory : " << error;
    return Result::RUN_LOCALLY;
  }

  UniquePtr<net::NetworkService> service;
  auto connection = Connect(socket_path, config, &service);
  if (!connection) {
    return Result::RUN_LOCALLY;
  }

  UniquePtr<base::proto::Local> message(new base::proto::Local);
  message->set_user_id(getuid());
  message->set_current_dir(current_dir);
  for (const auto& arg : args) {
    message->add_args(arg);
  }
  for (char** variable = environ; *variable; ++variable) {
    message->add_env(*variable);
  }

  net::proto::Status status;
  if (!connection->SendSync(std::move(message), &status)) {
    LOG(ERROR) << "Failed to send message: " << status.description();
    return Result::RUN_LOCALLY;
  }

  net::Connection::Message reply;
  if (!connection->ReadSync(&reply, &status)) {
    LOG(ERROR) << "Failed to read message: " << status.description();
    return Result::RUN_LOCALLY;
  }

  if (!reply.HasExtension(net::proto::Status::extension)) {
    LOG(ERROR) << "Message from daemon has wrong format : no Status extension";
    return Result::RUN_LOCALLY;
  }

  status = reply.GetExtension(net::proto::Status::extension);
  switch (status.code()) {
    case net::proto::Status::OK:
      std::cerr << status.description();
      return Result::SUCCESS;
    case net::proto::Status::EXECUTION:
      std::cerr << status.description();
      return Result::FAILURE;
    default:
      LOG(VERBOSE) << "Daemon didn't handle the command: "
                   << status.description();
      return Result::RUN_LOCALLY;
  }
}

bool PrintStats(const String& socket_path,
                const proto::Configuration& config) {
  UniquePtr<net::NetworkService> service;
  auto connection = Connect(socket_path, config, &service);
  if (!connection) {
    std::cerr << "Daemon isn't running on " << socket_path << std::endl;
    return false;
  }

  net::proto::Status status;
  if (!connection->SendSync(std::make_unique<perf::proto::Report>(),
                            &status)) {
    std::cerr << "Failed to send request: " << status.description()
              << std::endl;
    return false;
  }

  net::Connection::Message reply;
  if (!connection->ReadSync(&reply, &status) ||
      !reply.HasExtension(perf::proto::Report::extension)) {
    std::cerr << "Failed to read statistics: " << status.description()
              << std::endl;
    return false;
  }

  for (const auto& metric :
       reply.GetExtension(perf::proto::Report::extension).metric()) {
    std::cout << perf::proto::Metric::Name_Name(metric.name()) << " "
              << metric.value() << std::endl;
  }

  return true;
}

namespace {

String HumanSize(ui64 size) {
  const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double value = size;
  size_t unit = 0;
  while (value >= 1024 && unit + 1 < std::size(units)) {
    value /= 1024;
    ++unit;
  }
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(unit ? 1 : 0) << value << " "
         << units[unit];
  return stream.str();
}

String TagName(const String& tag) {
  return tag.empty() ? "(no tag)" : tag;
}

bool RequestTags(const String& socket_path, const proto::Configuration& config,
                 UniquePtr<base::proto::TagsRequest> request,
                 base::proto::TagsReport* report) {
  UniquePtr<net::NetworkService> service;
  auto connection = Connect(socket_path, config, &service);
  if (!connection) {
    std::cerr << "Daemon isn't running on " << socket_path << std::endl;
    return false;
  }

  net::proto::Status status;
  if (!connection->SendSync(std::move(request), &status)) {
    std::cerr << "Failed to send request: " << status.description()
              << std::endl;
    return false;
  }

  net::Connection::Message reply;
  if (!connection->ReadSync(&reply, &status)) {
    std::cerr << "Failed to read reply: " << status.description()
              << std::endl;
    return false;
  }
  if (!reply.HasExtension(base::proto::TagsReport::extension)) {
    if (reply.HasExtension(net::proto::Status::extension)) {
      std::cerr << reply.GetExtension(net::proto::Status::extension)
                       .description()
                << std::endl;
    } else {
      std::cerr << "Unexpected reply of the daemon" << std::endl;
    }
    return false;
  }

  report->CopyFrom(reply.GetExtension(base::proto::TagsReport::extension));
  return true;
}

}  // namespace

bool PrintTagStats(const String& socket_path,
                   const proto::Configuration& config) {
  base::proto::TagsReport report;
  if (!RequestTags(socket_path, config,
                   std::make_unique<base::proto::TagsRequest>(), &report)) {
    return false;
  }

  size_t width = 8;
  for (const auto& tag : report.tags()) {
    width = std::max(width, TagName(tag.name()).size());
  }

  std::cout << std::left << std::setw(width) << "tag" << std::right
            << std::setw(10) << "entries" << std::setw(12) << "size"
            << std::setw(12) << "exclusive" << std::setw(12) << "excl. size"
            << std::endl;
  for (const auto& tag : report.tags()) {
    std::cout << std::left << std::setw(width) << TagName(tag.name())
              << std::right << std::setw(10) << tag.entries() << std::setw(12)
              << HumanSize(tag.size()) << std::setw(12)
              << tag.exclusive_entries() << std::setw(12)
              << HumanSize(tag.exclusive_size()) << std::endl;
  }

  if (report.overlaps_size()) {
    std::cout << std::endl << "Shared by the tags:" << std::endl;
    for (const auto& overlap : report.overlaps()) {
      std::cout << "  " << TagName(overlap.first()) << " & "
                << TagName(overlap.second()) << ": "
                << HumanSize(overlap.size()) << std::endl;
    }
  }

  return true;
}

bool ForgetTag(const String& socket_path, const proto::Configuration& config,
               const String& tag, bool force) {
  auto request = std::make_unique<base::proto::TagsRequest>();
  request->set_forget(tag);
  request->set_dry_run(!force);
  base::proto::TagsReport report;
  if (!RequestTags(socket_path, config, std::move(request), &report)) {
    return false;
  }

  std::cout << (force ? "Removed " : "Would remove ")
            << report.removed_entries() << " entries, "
            << HumanSize(report.freed_size()) << std::endl;
  std::cout << "Kept " << report.shared_entries()
            << " entries of other tags too, " << report.linked_entries()
            << " entries linked with build directories" << std::endl;
  if (!force) {
    std::cout << "Add --force to remove them" << std::endl;
  }
  return true;
}

}  // namespace client
}  // namespace dist_clang
