// Measures MMDB builder throughput: parallel parsing + insertion, then build.
//
//   benchmark_builder [--networks N] [--threads T] [--lists L] [--mode batch|single]
//
// "batch" parses lock-free and inserts in batches (what the compiler does);
// "single" locks the trie for every network (the previous behaviour).
#include "mmdb_builder/builder.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace mmdb_builder;

namespace {

struct Options {
  size_t networks = 2'000'000;
  unsigned threads = std::thread::hardware_concurrency();
  size_t lists = 150;
  bool batch = true;
};

Options parse_options(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const char *value = i + 1 < argc ? argv[i + 1] : nullptr;
    if (arg == "--networks" && value) options.networks = std::strtoull(value, nullptr, 10), ++i;
    else if (arg == "--threads" && value) options.threads = std::strtoul(value, nullptr, 10), ++i;
    else if (arg == "--lists" && value) options.lists = std::strtoull(value, nullptr, 10), ++i;
    else if (arg == "--mode" && value) options.batch = std::string_view(value) != "single", ++i;
    else {
      std::fprintf(stderr, "usage: %s [--networks N] [--threads T] [--lists L] [--mode batch|single]\n",
                   argv[0]);
      std::exit(2);
    }
  }
  if (options.threads == 0) options.threads = 4;
  return options;
}

// Deterministic pseudo-random network text shaped like FireHOL data: mostly
// single hosts, some /24, a few aggregated blocks.
std::string network_text(uint64_t seed) {
  uint64_t x = seed * 6364136223846793005ULL + 1442695040888963407ULL;
  x ^= x >> 29;
  const unsigned a = 1 + (x & 0xFF) % 223;
  const unsigned b = (x >> 8) & 0xFF;
  const unsigned c = (x >> 16) & 0xFF;
  const unsigned d = (x >> 24) & 0xFF;
  const unsigned roll = (x >> 32) % 100;
  const unsigned prefix = roll < 90 ? 32 : roll < 98 ? 24 : 16 + (x >> 40) % 8;
  std::string text = std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) +
                     "." + std::to_string(d);
  if (prefix != 32) text += "/" + std::to_string(prefix);
  return text;
}

double seconds_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

int main(int argc, char **argv) {
  const Options options = parse_options(argc, argv);

  // Text generation is excluded from the measurement.
  std::vector<std::string> networks;
  networks.reserve(options.networks);
  for (size_t i = 0; i < options.networks; ++i) networks.push_back(network_text(i));

  Builder builder(6, "Benchmark");
  builder.set_row_sort_key("file_name");
  std::vector<data_id_t> records;
  records.reserve(options.lists);
  for (size_t list = 0; list < options.lists; ++list) {
    Object obj;
    obj["file_name"] = Data(Array{Data("list" + std::to_string(list) + ".netset")});
    obj["category"] = Data(Array{Data(list % 2 ? "attacks" : "abuse")});
    records.push_back(builder.store_data(Data(obj)));
  }

  std::atomic<size_t> rejected{0};
  const auto insert_start = std::chrono::steady_clock::now();
  {
    std::vector<std::jthread> workers;
    for (unsigned t = 0; t < options.threads; ++t) {
      workers.emplace_back([&, t] {
        std::vector<NetworkEntry> pending;
        pending.reserve(32768);
        for (size_t i = t; i < networks.size(); i += options.threads) {
          const data_id_t record = records[i % records.size()];
          if (!options.batch) {
            if (!builder.insert_network(networks[i], record)) rejected++;
            continue;
          }
          NetworkEntry entry;
          if (!builder.parse_network(networks[i], record, entry)) {
            rejected++;
            continue;
          }
          pending.push_back(entry);
          if (pending.size() == pending.capacity()) {
            builder.insert_parsed(pending);
            pending.clear();
          }
        }
        builder.insert_parsed(pending);
      });
    }
  }
  const double insert_seconds = seconds_since(insert_start);

  const auto output = std::filesystem::temp_directory_path() / "benchmark_builder.mmdb";
  const auto build_start = std::chrono::steady_clock::now();
  const BuildStats stats = builder.build(output.string());
  const double build_seconds = seconds_since(build_start);
  std::filesystem::remove(output);

  std::printf("mode=%s threads=%u networks=%zu lists=%zu rejected=%zu\n",
              options.batch ? "batch" : "single", options.threads, options.networks,
              options.lists, rejected.load());
  std::printf("insert: %.3f s (%.0f networks/s)\n", insert_seconds,
              static_cast<double>(options.networks) / insert_seconds);
  std::printf("build:  %.3f s (%u nodes, %zu records, %.1f MiB)\n", build_seconds,
              stats.node_count, stats.data_records,
              static_cast<double>(stats.total_bytes) / (1024.0 * 1024.0));
  return 0;
}
