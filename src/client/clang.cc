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

  if (!reply.has_status()) {
    LOG(ERROR) << "Message from daemon has wrong format : no Status extension";
    return Result::RUN_LOCALLY;
  }

  status = reply.status();
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

namespace {

enum class Unit { COUNT, BYTES, MILLISECONDS };

struct StatLine {
  perf::proto::Metric::Name metric;
  const char* label;
  Unit unit = Unit::COUNT;
  // Hidden, while zero - e.g. the remote compilation, that isn't used.
  bool hide_zero = false;
  // The values of the same kind, shown after this one through " / ".
  Vector<perf::proto::Metric::Name> more = {};
};

struct StatGroup {
  String title;
  Vector<StatLine> lines;
  // The counts are shown with their part of all compilations.
  bool of_compilations = false;
};

String FormatCount(ui64 value) {
  String digits = std::to_string(value), result;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i && (digits.size() - i) % 3 == 0) {
      result.push_back(',');
    }
    result.push_back(digits[i]);
  }
  return result;
}

String FormatTime(ui64 ms) {
  std::ostringstream stream;
  if (ms < 1000) {
    stream << ms << " ms";
  } else if (ms < 60 * 1000) {
    stream << std::fixed << std::setprecision(1) << ms / 1000.0 << " s";
  } else if (ms < 3600 * 1000) {
    stream << ms / 60000 << " min " << std::setw(2) << std::setfill('0')
           << ms / 1000 % 60 << " s";
  } else {
    stream << ms / 3600000 << " h " << std::setw(2) << std::setfill('0')
           << ms / 60000 % 60 << " min";
  }
  return stream.str();
}

String HumanSize(ui64 size);
String HumanSizes(const Vector<ui64>& sizes);

}  // namespace

bool PrintStats(const String& socket_path, const proto::Configuration& config,
                bool raw) {
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
      !reply.has_report()) {
    std::cerr << "Failed to read statistics: " << status.description()
              << std::endl;
    return false;
  }

  const auto& report = reply.report();
  // For scripts: "NAME value".
  if (raw) {
    for (const auto& metric : report.metric()) {
      std::cout << perf::proto::Metric::Name_Name(metric.name()) << " "
                << metric.value() << std::endl;
    }
    return true;
  }

  HashMap<int, ui64> values;
  for (const auto& metric : report.metric()) {
    values[metric.name()] = metric.value();
  }

  using M = perf::proto::Metric;
  // All compilations, that the daemon has got: with the direct cache every
  // one is looked up in it, otherwise - in the simple cache.
  const ui64 compilations =
      std::max(values[M::DIRECT_CACHE_HIT] + values[M::DIRECT_CACHE_MISS],
               values[M::SIMPLE_CACHE_HIT] + values[M::SIMPLE_CACHE_MISS]) +
      values[M::UNSUPPORTED_COMMAND] + values[M::CACHE_LOOKUP_SKIPPED];
  const String cache_title = "Cache (" + FormatCount(compilations) +
                             " compilations since the start of the daemon)";
  // The own size counts against the limit.
  const String cache_size_title =
      "Cache size (" + HumanSize(values[M::CACHE_OWN_SIZE]) + " of " +
      (values[M::CACHE_MAX_SIZE] ? HumanSize(values[M::CACHE_MAX_SIZE])
                                 : String("unlimited")) +
      ")";
  const StatGroup groups[] = {
      {"Saved (times summed over parallel tasks)",
       {
           {M::TIME_SAVED_BY_DIRECT_HITS, "Compilation by direct hits",
            Unit::MILLISECONDS},
           {M::TIME_SAVED_BY_SIMPLE_HITS, "Compilation by simple hits",
            Unit::MILLISECONDS},
           // Only with |max_output_links| other than 1.
           {M::DISK_SAVED_BY_LINKS, "Disk: one copy for several builds",
            Unit::BYTES, true},
           {M::OUTPUT_WRITES_AVOIDED, "Writes of outputs avoided", Unit::BYTES},
       }},
      {"Lost (removed entries, compiled again - see ghosts_ttl)",
       {
           {M::EVICTED_NEEDED_AGAIN, "Compiled again after eviction"},
           {M::TIME_LOST_TO_EVICTION, "Time lost to eviction",
            Unit::MILLISECONDS},
           // The size of the entries, evicted since the lost ones.
           {M::EXTRA_SIZE_FOR_HALF_LOSSES,
            "Larger cache to keep 50/90/100%",
            Unit::BYTES,
            false,
            {M::EXTRA_SIZE_FOR_90_PERCENT_LOSSES,
             M::EXTRA_SIZE_FOR_ALL_LOSSES}},
           {M::FORGOTTEN_NEEDED_AGAIN, "Compiled again after --forget-*"},
           {M::TIME_LOST_TO_FORGETTING, "Time lost to --forget-*",
            Unit::MILLISECONDS},
           {M::GHOSTS, "Removed entries remembered"},
       }},
      {cache_title,
       {
           {M::DIRECT_CACHE_HIT, "Direct cache hits"},
           {M::DIRECT_CACHE_MISS, "Direct cache misses"},
           {M::SIMPLE_CACHE_HIT, "Simple cache hits"},
           {M::SIMPLE_CACHE_MISS, "Simple cache misses"},
           {M::CACHE_LOOKUP_SKIPPED, "Not looked up (time trace, --recache)",
            Unit::COUNT, true},
           {M::PARALLEL_PREPROCESSING_HIT, "Hits by parallel preprocessing",
            Unit::COUNT, true},
           {M::REMOTE_CACHE_HIT, "Hits in the cache of remotes", Unit::COUNT,
            true},
           {M::HIT_VERIFIED, "Hits verified by compiling again", Unit::COUNT,
            true},
           {M::HIT_VERIFICATION_FAILED, "Hits failed the verification",
            Unit::COUNT, true},
       },
       true},
      {"Compilations",
       {
           {M::LOCAL_TASK_DONE, "Compiled locally"},
           {M::REMOTE_TASK_DONE, "Compiled remotely", Unit::COUNT, true},
           {M::REMOTE_COMPILATION_FAILED, "Remote compilations failed",
            Unit::COUNT, true},
           {M::REMOTE_COMPILATION_REJECTED, "Remote compilations rejected",
            Unit::COUNT, true},
           {M::UNSUPPORTED_COMMAND, "Run as is (not supported)"},
           {M::TASK_CANCELED, "Canceled (the client is gone)"},
           {M::PREPROCESSINGS, "Preprocessed"},
           {M::PREPROCESSING_SKIPPED, "Preprocessing skipped on a miss"},
           {M::PARALLEL_PREPROCESSING, "Preprocessed during the compilation"},
           {M::WARNINGS_RECHECKED, "Warnings rechecked (-fsyntax-only)",
            Unit::COUNT, true},
       },
       true},
      {"Outputs",
       {
           {M::CACHE_OUTPUT_LINKED, "Restored as links (hard or reflink)"},
           {M::CACHE_OUTPUT_COPIED, "Restored as copies"},
           {M::CACHE_OUTPUT_UNCHANGED, "Unchanged after a hit (kept)"},
           {M::LOCAL_OUTPUT_UNCHANGED, "Unchanged after compiling (kept)"},
           {M::CACHE_BLOB_REUSED, "Stored once for several entries"},
           {M::CACHE_STORE_SKIPPED, "Not stored: inputs changed meanwhile"},
           {M::HASH_MISMATCH, "Hash mismatches of remote results",
            Unit::COUNT, true},
       }},
      {cache_size_title,
       {
           {M::CACHE_INCOMPATIBLE, "Not used: incompatible format",
            Unit::COUNT, true},
           {M::CACHE_ENTRIES, "Entries"},
           {M::CACHE_OWN_SIZE, "Own size", Unit::BYTES},
           {M::CACHE_SHARED_SIZE, "Linked with build directories (free)",
            Unit::BYTES},
           // The outputs of several entries - e.g. after the changes of the
           // headers, that don't change the objects.
           {M::DISK_SAVED_BY_DEDUPLICATION, "Stored once for several entries",
            Unit::BYTES},
           {M::CACHE_SIZE_ADDED, "Added", Unit::BYTES},
           {M::CACHE_SIZE_CLEANED, "Removed", Unit::BYTES},
       }},
      {"Time (summed over parallel tasks)",
       {
           {M::LOCAL_COMPILATION_TIME, "Local compilation",
            Unit::MILLISECONDS},
           {M::PREPROCESS_TIME, "Preprocessing", Unit::MILLISECONDS},
           {M::DIRECT_CACHE_LOOKUP_TIME, "Direct cache lookups",
            Unit::MILLISECONDS},
           {M::DIRECT_CACHE_UPDATE_TIME, "Direct cache updates",
            Unit::MILLISECONDS},
           {M::SIMPLE_CACHE_LOOKUP_TIME, "Simple cache lookups",
            Unit::MILLISECONDS},
           {M::SIMPLE_CACHE_UPDATE_TIME, "Simple cache updates",
            Unit::MILLISECONDS},
           {M::REMOTE_COMPILATION_TIME, "Remote compilation",
            Unit::MILLISECONDS, true},
           {M::REMOTE_RESOLVE_TIME, "Resolving remotes", Unit::MILLISECONDS,
            true},
           {M::REMOTE_CONNECT_TIME, "Connecting to remotes",
            Unit::MILLISECONDS, true},
           {M::REMOTE_TIME_WASTED, "Wasted on failed remotes",
            Unit::MILLISECONDS, true},
       }},
  };

  auto Format = [](Unit unit, ui64 value) {
    switch (unit) {
      case Unit::BYTES:
        return HumanSize(value);
      case Unit::MILLISECONDS:
        return FormatTime(value);
      case Unit::COUNT:
        break;
    }
    return FormatCount(value);
  };

  // The limit is in the title.
  HashSet<int> shown = {M::CACHE_MAX_SIZE};
  const int kLabelWidth = 40, kValueWidth = 14;
  for (const auto& group : groups) {
    // The groups are separated by the empty lines.
    if (&group != groups) {
      std::cout << std::endl;
    }
    std::cout << group.title << std::endl;
    for (size_t i = 0; i < group.lines.size(); ++i) {
      const auto& line = group.lines[i];
      shown.insert(line.metric);
      const ui64 value = values[line.metric];
      if (line.hide_zero && !value) {
        continue;
      }
      String formatted = Format(line.unit, value);
      Vector<ui64> sizes = {value};
      for (const auto metric : line.more) {
        shown.insert(metric);
        formatted += " / " + Format(line.unit, values[metric]);
        sizes.push_back(values[metric]);
      }
      // The same unit for all sizes - to fit them into the line.
      if (line.unit == Unit::BYTES && sizes.size() > 1) {
        formatted = HumanSizes(sizes);
      }
      // The long values take the place of the label - aligned to the right.
      const int label_width = std::max<int>(
          strlen(line.label) + 1,
          kLabelWidth + kValueWidth - static_cast<int>(formatted.size()));
      std::cout << "  " << std::left << std::setw(label_width) << line.label
                << std::right
                << std::setw(kLabelWidth + kValueWidth - label_width)
                << formatted;
      if (group.of_compilations && line.unit == Unit::COUNT &&
          compilations) {
        std::cout << "  " << std::fixed << std::setprecision(1)
                  << std::setw(5) << 100.0 * value / compilations << "%";
      }
      std::cout << std::endl;
    }
  }

  // Nothing is lost, if a new metric isn't described here yet.
  bool other = false;
  for (const auto& metric : report.metric()) {
    if (shown.contains(metric.name()) || !metric.value()) {
      continue;
    }
    if (!other) {
      std::cout << std::endl << "Other" << std::endl;
      other = true;
    }
    std::cout << "  " << std::left << std::setw(kLabelWidth)
              << perf::proto::Metric::Name_Name(metric.name()) << std::right
              << std::setw(kValueWidth) << FormatCount(metric.value())
              << std::endl;
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

// The sizes in the same unit - of the largest one: "1.5 / 20.0 / 30.0 MiB".
String HumanSizes(const Vector<ui64>& sizes) {
  const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double max = *std::max_element(sizes.begin(), sizes.end());
  size_t unit = 0;
  double scale = 1;
  while (max >= 1024 && unit + 1 < std::size(units)) {
    max /= 1024;
    scale *= 1024;
    ++unit;
  }
  // Not more than 4 digits for every size.
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(unit && max < 99.95 ? 1 : 0);
  for (size_t i = 0; i < sizes.size(); ++i) {
    stream << (i ? " / " : "") << sizes[i] / scale;
  }
  stream << " " << units[unit];
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
  if (!reply.has_tags_report()) {
    if (reply.has_status()) {
      std::cerr << reply.status()
                       .description()
                << std::endl;
    } else {
      std::cerr << "Unexpected reply of the daemon" << std::endl;
    }
    return false;
  }

  report->CopyFrom(reply.tags_report());
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

  // "in builds" - linked with build directories: takes no space in the
  // cache, and isn't removed with the tag.
  std::cout << std::left << std::setw(width) << "tag" << std::right
            << std::setw(9) << "entries" << std::setw(12) << "size"
            << std::setw(12) << "in builds" << std::setw(12) << "exclusive"
            << std::setw(12) << "freeable" << std::setw(7) << "lost"
            << std::setw(14) << "lost time" << std::endl;
  for (const auto& tag : report.tags()) {
    std::cout << std::left << std::setw(width) << TagName(tag.name())
              << std::right << std::setw(9) << tag.entries() << std::setw(12)
              << HumanSize(tag.size()) << std::setw(12)
              << HumanSize(tag.linked_size()) << std::setw(12)
              << HumanSize(tag.exclusive_size()) << std::setw(12)
              << HumanSize(tag.freeable_size()) << std::setw(7)
              << tag.lost_entries() << std::setw(14)
              << FormatTime(tag.lost_time()) << std::endl;
  }

  if (report.overlaps_size()) {
    std::cout << std::endl << "Shared by the tags:" << std::endl;
    for (const auto& overlap : report.overlaps()) {
      std::cout << "  " << TagName(overlap.first()) << " & "
                << TagName(overlap.second()) << ": "
                << HumanSize(overlap.size()) << std::endl;
    }
  }

  std::cout << std::endl
            << "in builds - linked with build directories: takes no space in"
            << std::endl
            << "            the cache, and isn't removed with the tag;"
            << std::endl
            << "exclusive - used only by the tag;" << std::endl
            << "freeable  - removed by --forget-tag now;" << std::endl
            << "lost      - removed entries, that were compiled again, and"
            << std::endl
            << "            the time of these compilations." << std::endl;

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

bool ForgetGhosts(const String& socket_path,
                  const proto::Configuration& config) {
  auto request = std::make_unique<base::proto::TagsRequest>();
  request->set_forget_ghosts(true);
  base::proto::TagsReport report;
  if (!RequestTags(socket_path, config, std::move(request), &report)) {
    return false;
  }
  std::cout << "Forgot " << report.removed_ghosts() << " removed entries"
            << std::endl;
  return true;
}

}  // namespace client
}  // namespace dist_clang
