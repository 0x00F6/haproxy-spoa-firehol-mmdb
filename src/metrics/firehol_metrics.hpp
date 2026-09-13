#pragma once

#include <seastar/core/alien.hh>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace firehol_ipsets {

// Prometheus metrics for the FireHOL pipeline (Git synchronization and MMDB
// compilation). Producers run on scheduler or helper threads and only touch
// atomics or a mutex; Seastar reads the values on the shard that registered
// the groups. Per-blocklist gauges are rebuilt after every compilation so
// their series reflect exactly the last generated database.
class FireholMetrics {
public:
  struct BlocklistStat {
    std::string file_name;
    std::string category;
    uint64_t networks = 0;
  };

  // Compilation counters (compile_to_mmdb).
  std::atomic<uint64_t> compile_runs_total{0};
  std::atomic<uint64_t> compile_failures_total{0};
  std::atomic<uint64_t> compile_in_progress{0};
  std::atomic<double> compile_parse_seconds{0};
  std::atomic<double> compile_build_seconds{0};
  std::atomic<double> compile_total_seconds{0};
  std::atomic<uint64_t> compile_networks{0};
  std::atomic<uint64_t> compile_blocklists{0};
  std::atomic<uint64_t> compile_mmdb_bytes{0};
  std::atomic<uint64_t> compile_trie_nodes{0};
  std::atomic<uint64_t> compile_data_records{0};
  std::atomic<int64_t> compile_last_success_unix{0};

  // Git counters (GitRepository::prepare_repository).
  std::atomic<uint64_t> git_clones_total{0};
  std::atomic<uint64_t> git_fetches_total{0};
  std::atomic<uint64_t> git_failures_total{0};
  std::atomic<double> git_sync_seconds{0};
  std::atomic<int64_t> git_last_success_unix{0};

  // Replaces the per-blocklist series and republishes them on the registering
  // shard. Safe to call from any thread; a no-op before register_metrics().
  void set_blocklists(std::vector<BlocklistStat> stats);

  // Registers the static groups on the calling shard. Call once, on a reactor.
  void register_metrics();

  // Copy of the last published per-blocklist statistics.
  std::vector<BlocklistStat> blocklists() const;

private:
  void publish_blocklists();

  mutable std::mutex mutex_;
  std::vector<BlocklistStat> blocklists_;
  seastar::alien::instance *alien_ = nullptr;
  unsigned shard_ = 0;
  struct Groups;
  Groups *groups_ = nullptr; // owned by the registering shard, see metrics.cpp
};

// Process-wide instance shared by the compiler, Git wrapper and main().
FireholMetrics &metrics();

// Unix time in seconds, for *_timestamp_seconds gauges.
int64_t unix_now();

} // namespace firehol_ipsets
