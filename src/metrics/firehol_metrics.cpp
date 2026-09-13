#include "metrics/firehol_metrics.hpp"

#include <seastar/core/metrics.hh>
#include <seastar/core/reactor.hh>

#include <chrono>
#include <utility>

namespace firehol_ipsets {

namespace sm = seastar::metrics;

struct FireholMetrics::Groups {
  sm::metric_groups static_groups;
  sm::metric_groups blocklist_groups;
};

FireholMetrics &metrics() {
  static FireholMetrics instance;
  return instance;
}

int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::vector<FireholMetrics::BlocklistStat> FireholMetrics::blocklists() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return blocklists_;
}

void FireholMetrics::set_blocklists(std::vector<BlocklistStat> stats) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    blocklists_ = std::move(stats);
  }
  publish_blocklists();
}

void FireholMetrics::publish_blocklists() {
  if (!alien_) {
    return;
  }
  // Metric registration must happen on the owning reactor; run_on() queues
  // the rebuild there without blocking the producer thread.
  seastar::alien::run_on(*alien_, shard_, [this]() noexcept {
    static const sm::label file_label("file_name");
    static const sm::label category_label("category");
    const auto stats = blocklists();
    std::vector<sm::metric_definition> definitions;
    definitions.reserve(stats.size());
    for (const auto &stat : stats) {
      // Values are constants of the last compilation: no locking at scrape
      // time.
      definitions.push_back(
          sm::make_gauge(
              "networks",
              sm::description("Networks compiled from each blocklist "
                              "into the current MMDB"),
              {file_label(stat.file_name), category_label(stat.category)},
              [count = stat.networks] { return count; })
              .aggregate({sm::shard_label}));
    }
    groups_->blocklist_groups.clear();
    if (!definitions.empty()) {
      groups_->blocklist_groups.add_group("firehol_blocklist",
                                          std::move(definitions));
    }
  });
}

void FireholMetrics::register_metrics() {
  alien_ = &seastar::engine().alien();
  shard_ = seastar::this_shard_id();
  static Groups groups;
  groups_ = &groups;

  const auto counter = [](const char *name, const char *help,
                          std::atomic<uint64_t> &value) {
    return sm::make_counter(name, sm::description(help),
                            [&value] { return value.load(); })
        .aggregate({sm::shard_label});
  };
  const auto gauge = [](const char *name, const char *help, auto &value) {
    return sm::make_gauge(name, sm::description(help),
                          [&value] { return value.load(); })
        .aggregate({sm::shard_label});
  };

  groups.static_groups.add_group(
      "firehol_compile",
      {
          counter("runs_total",
                  "MMDB compilations started (startup and scheduled)",
                  compile_runs_total),
          counter("failures_total", "MMDB compilations that failed",
                  compile_failures_total),
          gauge("in_progress", "1 while a compilation is running",
                compile_in_progress),
          gauge("parse_seconds",
                "Blocklist parsing time of the last successful compilation",
                compile_parse_seconds),
          gauge("build_seconds",
                "Trie serialization time of the last successful compilation",
                compile_build_seconds),
          gauge("total_seconds",
                "End-to-end time of the last successful compilation",
                compile_total_seconds),
          gauge("networks", "Networks inserted into the current MMDB",
                compile_networks),
          gauge("blocklists", "Blocklist files compiled into the current MMDB",
                compile_blocklists),
          gauge("mmdb_bytes", "Size of the current MMDB in bytes",
                compile_mmdb_bytes),
          gauge("trie_nodes", "Search tree nodes in the current MMDB",
                compile_trie_nodes),
          gauge("data_records", "Distinct data records in the current MMDB",
                compile_data_records),
          gauge("last_success_timestamp_seconds",
                "Unix time of the last successful compilation, 0 if none",
                compile_last_success_unix),
      });
  groups.static_groups.add_group(
      "firehol_git",
      {
          counter("clones_total", "Fresh clones of the blocklist repository",
                  git_clones_total),
          counter("fetches_total",
                  "Fetch and reset operations on an existing checkout",
                  git_fetches_total),
          counter("failures_total", "Clone or fetch operations that failed",
                  git_failures_total),
          gauge("sync_seconds",
                "Duration of the last successful Git synchronization",
                git_sync_seconds),
          gauge(
              "last_success_timestamp_seconds",
              "Unix time of the last successful Git synchronization, 0 if none",
              git_last_success_unix),
      });
  // Series published before registration (fetch-and-create at startup).
  publish_blocklists();
}

} // namespace firehol_ipsets
