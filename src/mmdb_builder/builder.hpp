#pragma once

#include <array>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace mmdb_builder {

class Data;

// C++ struct for arbitrary JSON-like tree
using Object = std::map<std::string, Data, std::less<>>;
using Array = std::vector<Data>;

class Data {
public:
  std::variant<std::monostate, std::string, std::vector<uint8_t>, uint16_t,
               uint32_t, int32_t, uint64_t, bool, float, double, Object, Array>
      value;

  Data() = default;
  Data(const char *s) : value(std::string(s)) {}
  Data(std::string s) : value(std::move(s)) {}
  Data(uint16_t v) : value(v) {}
  Data(uint32_t v) : value(v) {}
  Data(int32_t v) : value(v) {}
  Data(uint64_t v) : value(v) {}
  Data(bool v) : value(v) {}
  Data(float v) : value(v) {}
  Data(double v) : value(v) {}
  Data(Object o) : value(std::move(o)) {}
  Data(Array a) : value(std::move(a)) {}
};

using data_id_t = uint32_t;

// Efficient pooled internal representations to allow fast hashing and zero-copy
// merge
struct InternalMap {
  std::vector<std::pair<data_id_t, data_id_t>> fields;
  bool operator==(const InternalMap &o) const { return fields == o.fields; }
};

struct InternalArray {
  std::vector<data_id_t> elements;
  bool operator==(const InternalArray &o) const {
    return elements == o.elements;
  }
};

using InternalData =
    std::variant<std::monostate, std::string_view, std::vector<uint8_t>,
                 uint16_t, uint32_t, int32_t, uint64_t, bool, float, double,
                 InternalMap, InternalArray>;

struct InternalDataHash {
  std::size_t operator()(const InternalData &d) const;
};

struct StringHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view s) const {
    return std::hash<std::string_view>{}(s);
  }
};

class DataPool {
public:
  std::unordered_set<std::string, StringHash, std::equal_to<>> strings_;
  std::unordered_map<InternalData, data_id_t, InternalDataHash> data_to_id_;
  std::vector<InternalData> id_to_data_;

  DataPool();

  std::string_view intern_string(std::string_view s);
  data_id_t intern(const InternalData &d);
  data_id_t store(const Data &external_data);

  // Merges two records. Arrays are concatenated; maps are merged key by key;
  // for other types `b` wins. When both maps are "columnar" (same keys, every
  // value an array, all columns of equal length), rows are concatenated,
  // deduplicated and sorted by `row_sort_key` (then by the remaining
  // columns), so the result is independent of insertion order.
  data_id_t deep_merge(data_id_t a, data_id_t b);
  void set_row_sort_key(std::string key) { row_sort_key_ = std::move(key); }

  // Total order on values by content (type, then value, recursively), so
  // callers can lay out the data section independently of interning order.
  bool less_data(data_id_t a, data_id_t b) const;

private:
  std::string row_sort_key_;
  bool columnar_rows(const InternalMap &m, size_t &rows) const;
  data_id_t merge_columnar(const InternalMap &ma, size_t rows_a,
                           const InternalMap &mb, size_t rows_b);
};

struct TrieNode {
  uint32_t children[2]{0, 0};
  data_id_t data{0};
};

// A network parsed by Builder::parse_network, ready for insertion. IPv4
// networks of an IPv6 database are mapped to ::a.b.c.d/(96 + n).
struct NetworkEntry {
  std::array<uint8_t, 16> bits{};
  uint8_t prefix_len = 0;
  data_id_t data = 0;
};

struct BuildStats {
  uint32_t node_count = 0;
  size_t data_records = 0;
  size_t data_section_bytes = 0;
  size_t total_bytes = 0;
};

class Builder {
  uint32_t ip_version_;
  std::string database_type_;
  uint64_t build_epoch_;
  std::vector<TrieNode> nodes_;
  DataPool pool_;
  std::unordered_map<data_id_t, uint32_t> emitted_;

  // Thread safety for concurrent inserts
  std::mutex global_mutex_;
  std::mutex pool_mutex_;

  // Node of ::/96, under which every IPv4 network of an IPv6 tree lives.
  // Created on first use; 0 (the root) means "not created yet".
  uint32_t ipv4_root_ = 0;

  uint32_t alloc_node();
  // Walks `bits` from `from` (at depth `from_depth`) down to `to_depth`,
  // creating and splitting nodes as needed. Returns the node reached.
  uint32_t descend(uint32_t from, uint8_t from_depth, const uint8_t *bits,
                   uint8_t to_depth);
  void insert_bits(const uint8_t *bits, uint8_t prefix_len, data_id_t data_id);
  void merge_subtree(uint32_t node_idx, data_id_t new_data);

  data_id_t prune_tree_parallel(uint32_t node_idx, int depth_limit);
  uint32_t assign_node_ids(uint32_t current, uint32_t &next_id,
                           std::vector<uint32_t> &mapping,
                           std::vector<uint32_t> &ordered);
  void collect_used_data_parallel(uint32_t current, int depth_limit,
                                  std::unordered_set<data_id_t> &used);

  void encode_ctrl_and_size(std::vector<uint8_t> &out, uint8_t type,
                            uint32_t size);
  void encode_pointer(std::vector<uint8_t> &out, uint32_t offset);
  void encode_uint(std::vector<uint8_t> &out, uint8_t type, uint64_t val);
  void encode_int(std::vector<uint8_t> &out, int32_t val);
  void serialize_data(data_id_t id, std::vector<uint8_t> &out);
  void write_metadata(std::vector<uint8_t> &out, uint32_t node_count);

public:
  Builder(uint32_t ip_version = 6, std::string db_type = "IP-Reputation");

  void set_build_epoch(uint64_t epoch) { build_epoch_ = epoch; }

  // Column used to order merged rows of columnar records (see DataPool).
  void set_row_sort_key(std::string key) {
    pool_.set_row_sort_key(std::move(key));
  }

  // Pre-sizes node storage; a good estimate avoids copying the trie while it
  // grows (about 2.2 nodes per network for FireHOL data).
  void reserve_nodes(size_t count) { nodes_.reserve(count); }

  // Thread-safe data insertion returning data reference
  data_id_t store_data(const Data &data);

  // Parses "address[/prefix]" for this database's IP version without
  // touching shared state, so workers can parse concurrently and lock-free.
  // Returns false for malformed input, a wrong address family or a prefix
  // longer than the address.
  [[nodiscard]] bool parse_network(std::string_view cidr, data_id_t data,
                                   NetworkEntry &out) const noexcept;

  // Inserts already parsed networks under a single lock. Batching amortizes
  // the trie lock over many networks; parsing stays outside of it.
  void insert_parsed(std::span<const NetworkEntry> entries);

  // Thread-safe parse-and-insert of one network (parse_network +
  // insert_parsed).
  bool insert_network(std::string_view cidr, data_id_t data_id);
  bool insert_network(std::string_view cidr, const Data &data);

  // Finalizes construction and streams the generated MMDB to the specified file
  // directly
  BuildStats build(const std::string &output_filename);
};

} // namespace mmdb_builder
