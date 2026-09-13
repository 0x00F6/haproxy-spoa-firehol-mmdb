// The MMDB builder must produce the same tree and data section regardless of
// insertion order, batching and the number of parsing threads. Only the
// metadata (build epoch) may differ between two builds.
#include "mmdb_builder/builder.hpp"

#include <maxminddb.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace mmdb_builder;
namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, std::string_view what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
  }
}

struct Source {
  std::string cidr;
  std::string list;
  std::string category;
};

// Columnar record like the FireHOL compiler emits.
Data record(const Source &source) {
  Object obj;
  obj["file_name"] = Data(Array{Data(source.list)});
  obj["category"] = Data(Array{Data(source.category)});
  return Data(obj);
}

std::vector<Source> make_sources() {
  std::vector<Source> sources;
  // Overlapping networks across lists so that merges happen at several depths.
  const char *categories[] = {"attacks", "unroutable", "abuse"};
  for (int list = 0; list < 6; ++list) {
    const std::string name = "list" + std::to_string(list) + ".netset";
    for (int i = 0; i < 300; ++i) {
      const int a = 10 + (i * 7 + list) % 3;
      const int b = (i * 13) % 256;
      const int c = (i * 31 + list * 5) % 256;
      const int prefix = 20 + (i % 13);
      sources.push_back({std::to_string(a) + "." + std::to_string(b) + "." +
                             std::to_string(c) + ".0/" + std::to_string(prefix),
                         name, categories[(i + list) % 3]});
    }
    sources.push_back({"2001:db8:" + std::to_string(list) + "::/48", name, "ipv6"});
    sources.push_back({"192.0.2." + std::to_string(list * 10), name, "hosts"});
  }
  return sources;
}

// Bytes before the metadata marker: search tree, separator and data section.
std::string payload_without_metadata(const fs::path &file) {
  std::ifstream input(file, std::ios::binary);
  std::string content{std::istreambuf_iterator<char>(input), {}};
  const auto marker = content.rfind("\xAB\xCD\xEFMaxMind.com");
  check(marker != std::string::npos, "metadata marker present");
  return content.substr(0, marker);
}

// Inserts `sources` in the given order using `threads` workers and `batch`
// networks per insert_parsed call (0 = one insert_network per entry).
fs::path build(const std::vector<Source> &sources, unsigned threads, size_t batch,
               const std::string &tag) {
  Builder builder(6, "DeterminismTest");
  builder.set_row_sort_key("file_name");
  std::vector<data_id_t> ids;
  ids.reserve(sources.size());
  for (const auto &source : sources) {
    ids.push_back(builder.store_data(record(source)));
  }

  std::vector<std::jthread> workers;
  for (unsigned t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      std::vector<NetworkEntry> pending;
      for (size_t i = t; i < sources.size(); i += threads) {
        if (batch == 0) {
          check(builder.insert_network(sources[i].cidr, ids[i]), "insert_network accepts input");
          continue;
        }
        NetworkEntry entry;
        check(builder.parse_network(sources[i].cidr, ids[i], entry), "parse_network accepts input");
        pending.push_back(entry);
        if (pending.size() == batch) {
          builder.insert_parsed(pending);
          pending.clear();
        }
      }
      builder.insert_parsed(pending);
    });
  }
  workers.clear();

  const fs::path output = fs::temp_directory_path() / ("builder-determinism-" + tag + ".mmdb");
  const BuildStats stats = builder.build(output.string());
  check(stats.node_count > 0 && stats.data_records > 0, "build reports statistics");
  check(stats.total_bytes == fs::file_size(output), "total_bytes matches the file size");
  return output;
}

void check_lookup(const fs::path &file) {
  MMDB_s mmdb{};
  check(MMDB_open(file.c_str(), MMDB_MODE_MMAP, &mmdb) == MMDB_SUCCESS, "open output");
  int gai_error = 0;
  int mmdb_error = 0;
  auto result = MMDB_lookup_string(&mmdb, "192.0.2.10", &gai_error, &mmdb_error);
  check(gai_error == 0 && mmdb_error == MMDB_SUCCESS && result.found_entry, "host entry found");
  MMDB_entry_data_s entry{};
  check(MMDB_get_value(&result.entry, &entry, "file_name", "0", nullptr) == MMDB_SUCCESS &&
            entry.type == MMDB_DATA_TYPE_UTF8_STRING &&
            std::string_view(entry.utf8_string, entry.data_size) == "list1.netset",
        "host entry names its list");
  result = MMDB_lookup_string(&mmdb, "2001:db8:3::1", &gai_error, &mmdb_error);
  check(mmdb_error == MMDB_SUCCESS && result.found_entry, "IPv6 entry found");
  result = MMDB_lookup_string(&mmdb, "203.0.113.1", &gai_error, &mmdb_error);
  check(mmdb_error == MMDB_SUCCESS && !result.found_entry, "unlisted network absent");
  MMDB_close(&mmdb);
}

void test_invalid_networks() {
  Builder builder(6, "Invalid");
  NetworkEntry entry;
  check(!builder.parse_network("10.0.0.0/33", 1, entry), "IPv4 prefix > 32 rejected");
  check(!builder.parse_network("::/129", 1, entry), "IPv6 prefix > 128 rejected");
  check(!builder.parse_network("10.0.0.0/", 1, entry), "empty prefix rejected");
  check(!builder.parse_network("10.0.0.0/2a", 1, entry), "non-numeric prefix rejected");
  check(!builder.parse_network("not-a-network", 1, entry), "garbage rejected");
  check(builder.parse_network("10.0.0.1", 1, entry) && entry.prefix_len == 128 &&
            entry.bits[12] == 10 && entry.bits[15] == 1,
        "bare IPv4 maps to ::a.b.c.d/128");
  check(builder.parse_network("10.0.0.0/8", 1, entry) && entry.prefix_len == 104,
        "IPv4 prefix is offset by 96 in an IPv6 tree");
  Builder v4(4, "V4");
  check(!v4.parse_network("::1", 1, entry), "IPv6 rejected by an IPv4 tree");
  check(v4.parse_network("10.0.0.0/8", 1, entry) && entry.prefix_len == 8 && entry.bits[0] == 10,
        "IPv4 tree keeps native prefixes");
}

} // namespace

int main() {
  const auto sources = make_sources();
  auto reversed = sources;
  std::reverse(reversed.begin(), reversed.end());
  auto shuffled = sources;
  std::shuffle(shuffled.begin(), shuffled.end(), std::mt19937{42});

  const auto a = build(sources, 1, 0, "a");
  const auto b = build(reversed, 4, 7, "b");
  const auto c = build(shuffled, 8, 1000, "c");

  const auto payload_a = payload_without_metadata(a);
  check(payload_a == payload_without_metadata(b), "reverse order + batches gives identical output");
  check(payload_a == payload_without_metadata(c), "shuffled order + 8 threads gives identical output");
  check_lookup(a);
  check_lookup(c);
  test_invalid_networks();

  for (const auto &file : {a, b, c}) {
    fs::remove(file);
  }
  if (failures == 0) {
    std::cout << "All builder determinism tests passed!\n";
    return 0;
  }
  return 1;
}
