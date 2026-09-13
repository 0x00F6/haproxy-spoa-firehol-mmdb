#include "compiler.hpp"
#include "metrics/firehol_metrics.hpp"
#include "mmdb_builder/builder.hpp"
#include "utils/string_utils.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <seastar/util/log.hh>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace mmdb_builder;

namespace firehol_ipsets {

static seastar::logger fhlog("firehol");

namespace {

using spoe::utils::group_thousands;
using spoe::utils::trim;

// Networks are handed to the builder in batches: one lock per batch instead
// of one per network, while parsing runs fully in parallel.
constexpr size_t kInsertBatchSize = 32768;

double elapsed_ms(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - since)
      .count();
}

std::string group_thousands_ms(double value) {
  return group_thousands(static_cast<uint64_t>(value < 0 ? 0 : value + 0.5));
}

struct Metadata {
  std::string category;
  std::string maintainer_url;
  std::string maintainer;
  std::string list_source_url;
  std::string source_file_date_rfc3339;
};

std::string parse_source_file_date(std::string_view raw) {
  struct tm tm = {};
  const std::string text(raw);
  if (strptime(text.c_str(), "%a %b %e %H:%M:%S UTC %Y", &tm) != nullptr) {
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+00:00", &tm);
    return buf;
  }
  throw std::runtime_error("failed to parse datetime " + text);
}

std::string read_file(const fs::path &filepath) {
  std::ifstream file(filepath, std::ios::binary | std::ios::ate);
  if (!file) {
    throw std::runtime_error("Could not open blocklist: " + filepath.string());
  }
  const auto size = static_cast<std::streamsize>(file.tellg());
  if (size < 0) {
    throw std::runtime_error("Could not determine blocklist size: " +
                             filepath.string());
  }
  std::string content(static_cast<size_t>(size), '\0');
  if (size > 0) {
    file.seekg(0, std::ios::beg);
    if (!file.read(content.data(), size)) {
      throw std::runtime_error("Could not read blocklist: " +
                               filepath.string());
    }
  }
  return content;
}

std::runtime_error parse_error(const fs::path &filepath, int line_number,
                               std::string_view what) {
  return std::runtime_error(filepath.string() + ":" +
                            std::to_string(line_number) + ": " +
                            std::string(what));
}

// Result of parsing one blocklist: networks inserted and the category its
// metadata declares (the last one seen when a file declares several).
struct ParsedBlocklist {
  size_t networks = 0;
  std::string category;
};

// Parses one blocklist and inserts its networks.
ParsedBlocklist process_file(const fs::path &filepath, Builder &builder) {
  const std::string stem = filepath.stem().string();
  const std::string file_name = filepath.filename().string();
  fhlog.trace("Parsing blocklist {}", filepath.string());

  const std::string content = read_file(filepath);
  if (content.empty()) {
    fhlog.debug("Skipping empty blocklist {}", filepath.string());
    return {};
  }

  Metadata metadata;
  data_id_t did = 0;
  int network_count = 0;
  size_t total_networks = 0;
  int metadata_sections = 0;

  // Every record is columnar: each field is a one-element array. Networks
  // listed by several blocklists merge into parallel arrays where index i
  // describes the i-th source (see DataPool::deep_merge). Missing values are
  // kept as "" so the columns stay aligned.
  auto update_did = [&]() {
    auto column = [](const std::string &value) {
      return Data(Array{Data(value)});
    };
    Object obj;
    obj["file_name"] = column(file_name);
    obj["category"] = column(metadata.category);
    obj["maintainer"] = column(metadata.maintainer);
    obj["maintainer_url"] = column(metadata.maintainer_url);
    obj["list_source_url"] = column(metadata.list_source_url);
    obj["source_file_date_rfc3339"] = column(metadata.source_file_date_rfc3339);
    did = builder.store_data(Data(std::move(obj)));
    network_count = 0;
    metadata_sections++;
  };

  std::vector<NetworkEntry> batch;
  batch.reserve(std::min(kInsertBatchSize, content.size() / 8 + 1));
  auto flush = [&]() {
    builder.insert_parsed(batch);
    batch.clear();
  };

  std::string_view sv(content);
  size_t pos = 0;
  int line_number = 0;

  while (pos < sv.size()) {
    size_t end = sv.find('\n', pos);
    if (end == std::string_view::npos)
      end = sv.size();

    std::string_view line = sv.substr(pos, end - pos);
    pos = end + 1;
    line_number++;

    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    if (line.empty())
      continue;

    if (line.front() == '#') {
      const std::string_view comment = line.substr(1);
      const size_t colon = comment.find(':');
      if (colon == std::string_view::npos)
        continue;
      const std::string_view key = trim(comment.substr(0, colon));
      const std::string_view value = trim(comment.substr(colon + 1));

      std::string *slot = nullptr;
      std::string slot_val;
      if (key == "Category") {
        slot = &metadata.category;
        slot_val = std::string(value);
      } else if (key == "Maintainer URL") {
        slot = &metadata.maintainer_url;
        slot_val = std::string(value);
      } else if (key == "Maintainer") {
        slot = &metadata.maintainer;
        slot_val = std::string(value);
      } else if (key == "List source URL") {
        slot = &metadata.list_source_url;
        slot_val = std::string(value);
      } else if (key == "Source File Date") {
        slot = &metadata.source_file_date_rfc3339;
        try {
          slot_val = parse_source_file_date(value);
        } catch (...) {
          throw parse_error(filepath, line_number, "invalid Source File Date");
        }
      }
      if (slot && *slot != slot_val) {
        // Metadata changed after networks were emitted: start a new record.
        if (network_count > 0)
          did = 0;
        *slot = std::move(slot_val);
      }
      continue;
    }

    const std::string_view entry = trim(line);
    if (entry.empty())
      continue;

    if (did == 0)
      update_did();

    NetworkEntry parsed;
    if (!builder.parse_network(entry, did, parsed)) {
      throw parse_error(filepath, line_number,
                        "invalid entry " + std::string(entry));
    }
    batch.push_back(parsed);
    network_count++;
    total_networks++;
    if (batch.size() == kInsertBatchSize)
      flush();
  }
  flush();

  if (metadata.category.empty()) {
    fhlog.warn("Blocklist {} declares no Category", filepath.string());
  }
  fhlog.debug(
      "Parsed {}: {} networks, {} lines, {} metadata section(s), category={}",
      stem, total_networks, line_number, metadata_sections,
      metadata.category.empty() ? "(none)" : metadata.category);
  return {total_networks, std::move(metadata.category)};
}

// Marks a compilation as running for the duration of compile_to_mmdb().
class CompileRun {
public:
  CompileRun() {
    metrics().compile_runs_total.fetch_add(1, std::memory_order_relaxed);
    metrics().compile_in_progress.store(1, std::memory_order_relaxed);
  }
  ~CompileRun() {
    if (!succeeded_)
      metrics().compile_failures_total.fetch_add(1, std::memory_order_relaxed);
    metrics().compile_in_progress.store(0, std::memory_order_relaxed);
  }
  void succeeded() noexcept { succeeded_ = true; }

private:
  bool succeeded_ = false;
};

// The builder accepts a filename, so keep the unique temporary filename alive
// until the completed database has been published beside its destination.
class TemporaryMmdb {
public:
  explicit TemporaryMmdb(const fs::path &target_path) {
    auto temporary_path = target_path;
    temporary_path += ".tmp.XXXXXX";
    filename_ = temporary_path.string();
    const int fd = ::mkstemp(filename_.data());
    if (fd < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "Could not create temporary MMDB");
    }
    if (::close(fd) != 0) {
      const int error = errno;
      std::remove(filename_.c_str());
      throw std::system_error(error, std::generic_category(),
                              "Could not close temporary MMDB");
    }
  }

  ~TemporaryMmdb() { std::remove(filename_.c_str()); }

  TemporaryMmdb(const TemporaryMmdb &) = delete;
  TemporaryMmdb &operator=(const TemporaryMmdb &) = delete;

  const std::string &filename() const { return filename_; }

private:
  std::string filename_;
};

struct BlocklistFile {
  fs::path path;
  uintmax_t size;
};

std::vector<BlocklistFile> list_blocklists(const fs::path &repository_path,
                                           size_t &ipset_count,
                                           size_t &netset_count) {
  std::vector<BlocklistFile> files;
  for (const auto &entry : fs::directory_iterator(repository_path)) {
    if (!entry.is_regular_file())
      continue;
    const auto ext = entry.path().extension();
    if (ext != ".ipset" && ext != ".netset")
      continue;
    (ext == ".ipset" ? ipset_count : netset_count)++;
    files.push_back({entry.path(), entry.file_size()});
  }
  // Largest files first: workers pull the next file dynamically, so this
  // longest-processing-time-first order minimizes the parsing tail.
  std::sort(files.begin(), files.end(),
            [](const BlocklistFile &a, const BlocklistFile &b) {
              if (a.size != b.size)
                return a.size > b.size;
              return a.path < b.path;
            });
  return files;
}

} // namespace

void compile_to_mmdb(const fs::path &target_mmdb_path,
                     const fs::path &repository_path,
                     uint64_t last_commit_unix) {
  CompileRun run;
  const auto started = std::chrono::steady_clock::now();
  fhlog.info("Compiling FireHOL blocklists from {} into {}",
             repository_path.string(), target_mmdb_path.string());

  size_t ipset_count = 0;
  size_t netset_count = 0;
  const auto files =
      list_blocklists(repository_path, ipset_count, netset_count);
  if (files.empty()) {
    fhlog.warn("No .ipset or .netset file found in {}",
               repository_path.string());
  }

  Builder builder(6, "FireHOL-IP-Reputation");
  builder.set_row_sort_key("file_name");
  // Roughly one network per 10 bytes of blocklist text and 2.2 trie nodes
  // per network; over-reserving only costs untouched virtual memory.
  uintmax_t total_bytes = 0;
  for (const auto &file : files)
    total_bytes += file.size;
  builder.reserve_nodes(static_cast<size_t>(total_bytes / 10 * 5 / 2) + 1024);

  size_t num_threads = std::thread::hardware_concurrency();
  if (num_threads == 0)
    num_threads = 4;
  num_threads = std::min(num_threads, files.size());
  fhlog.info(
      "Parsing {} blocklist(s) ({} .ipset, {} .netset) with {} thread(s)",
      files.size(), ipset_count, netset_count, num_threads);

  std::atomic<size_t> next_file{0};
  std::atomic<size_t> total_networks{0};
  // One slot per file, written by the worker that parsed it: no shared
  // mutation.
  std::vector<FireholMetrics::BlocklistStat> blocklist_stats(files.size());
  std::vector<std::exception_ptr> errors(num_threads);
  {
    // jthread also joins already started workers if thread creation fails.
    std::vector<std::jthread> workers;
    workers.reserve(num_threads);
    for (size_t t = 0; t < num_threads; ++t) {
      workers.emplace_back([&, t]() {
        size_t parsed = 0;
        try {
          for (size_t i = next_file.fetch_add(1, std::memory_order_relaxed);
               i < files.size();
               i = next_file.fetch_add(1, std::memory_order_relaxed)) {
            ParsedBlocklist result = process_file(files[i].path, builder);
            parsed += result.networks;
            blocklist_stats[i] = {files[i].path.filename().string(),
                                  std::move(result.category), result.networks};
          }
        } catch (...) {
          errors[t] = std::current_exception();
        }
        total_networks.fetch_add(parsed, std::memory_order_relaxed);
      });
    }
  }

  size_t failed_workers = 0;
  std::exception_ptr first_error;
  for (const auto &error : errors) {
    if (!error)
      continue;
    failed_workers++;
    if (!first_error)
      first_error = error;
    try {
      std::rethrow_exception(error);
    } catch (const std::exception &e) {
      fhlog.error("Blocklist parsing failed: {}", e.what());
    } catch (...) {
      fhlog.error("Blocklist parsing failed with an unknown exception");
    }
  }
  if (first_error) {
    fhlog.error("Aborting MMDB generation: {} worker(s) failed after {} ms",
                failed_workers, group_thousands_ms(elapsed_ms(started)));
    std::rethrow_exception(first_error);
  }
  const double parse_ms = elapsed_ms(started);
  fhlog.info("Parsed {} networks from {} blocklist(s) in {} ms",
             group_thousands(total_networks.load()), files.size(),
             group_thousands_ms(parse_ms));

  const auto build_started = std::chrono::steady_clock::now();
  TemporaryMmdb output(target_mmdb_path);
  fhlog.debug("Building MMDB into temporary file {}", output.filename());
  const BuildStats stats = builder.build(output.filename());
  const double build_ms = elapsed_ms(build_started);
  fhlog.info("MMDB built in {} ms ({:.1f} MiB, {} nodes, {} data records)",
             group_thousands_ms(build_ms),
             static_cast<double>(stats.total_bytes) / (1024.0 * 1024.0),
             group_thousands(stats.node_count),
             group_thousands(stats.data_records));

  fs::rename(output.filename(), target_mmdb_path);
  const double total_ms = elapsed_ms(started);
  fhlog.info("MMDB published at {} (total {} ms)", target_mmdb_path.string(),
             group_thousands_ms(total_ms));

  // Publish the statistics of the database that is now live.
  auto &m = metrics();
  m.compile_parse_seconds.store(parse_ms / 1000.0, std::memory_order_relaxed);
  m.compile_build_seconds.store(build_ms / 1000.0, std::memory_order_relaxed);
  m.compile_total_seconds.store(total_ms / 1000.0, std::memory_order_relaxed);
  m.compile_networks.store(total_networks.load(), std::memory_order_relaxed);
  m.compile_blocklists.store(files.size(), std::memory_order_relaxed);
  m.compile_mmdb_bytes.store(stats.total_bytes, std::memory_order_relaxed);
  m.compile_trie_nodes.store(stats.node_count, std::memory_order_relaxed);
  m.compile_data_records.store(stats.data_records, std::memory_order_relaxed);
  m.compile_last_success_unix.store(unix_now(), std::memory_order_relaxed);
  m.set_blocklists(std::move(blocklist_stats));
  run.succeeded();
}

} // namespace firehol_ipsets
