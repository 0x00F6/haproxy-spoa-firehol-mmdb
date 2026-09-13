#include "builder.hpp"
#include "utils/net_utils.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <ctime>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace mmdb_builder {

template <class T> inline void hash_combine(std::size_t &seed, const T &v) {
  seed ^= std::hash<T>{}(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

std::size_t InternalDataHash::operator()(const InternalData &d) const {
  std::size_t seed = d.index();
  std::visit(
      [&seed](auto &&arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
        } else if constexpr (std::is_same_v<T, std::string_view>) {
          hash_combine(seed, arg);
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
          for (auto v : arg)
            hash_combine(seed, v);
        } else if constexpr (std::is_same_v<T, InternalMap>) {
          for (const auto &kv : arg.fields) {
            hash_combine(seed, kv.first);
            hash_combine(seed, kv.second);
          }
        } else if constexpr (std::is_same_v<T, InternalArray>) {
          for (auto v : arg.elements)
            hash_combine(seed, v);
        } else {
          hash_combine(seed, arg);
        }
      },
      d);
  return seed;
}

DataPool::DataPool() {
  id_to_data_.emplace_back(std::monostate{}); // 0 reserved for empty
}

std::string_view DataPool::intern_string(std::string_view s) {
  auto it = strings_.find(s);
  if (it != strings_.end())
    return *it;
  auto [inserted, _] = strings_.emplace(s);
  return *inserted;
}

data_id_t DataPool::intern(const InternalData &d) {
  if (auto it = data_to_id_.find(d); it != data_to_id_.end()) {
    return it->second;
  }
  data_id_t id = id_to_data_.size();
  id_to_data_.push_back(d);
  data_to_id_.emplace(d, id);
  return id;
}

data_id_t DataPool::store(const Data &d) {
  return std::visit(
      [this](auto &&arg) -> data_id_t {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>)
          return 0;
        else if constexpr (std::is_same_v<T, std::string>) {
          return intern(InternalData{intern_string(arg)});
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
          return intern(InternalData{arg});
        } else if constexpr (std::is_same_v<T, Object>) {
          InternalMap m;
          m.fields.reserve(arg.size());
          for (const auto &kv : arg) {
            m.fields.push_back({intern(InternalData{intern_string(kv.first)}),
                                store(kv.second)});
          }
          std::sort(m.fields.begin(), m.fields.end(),
                    [this](const auto &a, const auto &b) {
                      return std::get<std::string_view>(id_to_data_[a.first]) <
                             std::get<std::string_view>(id_to_data_[b.first]);
                    });
          return intern(m);
        } else if constexpr (std::is_same_v<T, Array>) {
          InternalArray a;
          a.elements.reserve(arg.size());
          for (const auto &item : arg)
            a.elements.push_back(store(item));
          return intern(a);
        } else {
          return intern(InternalData{arg});
        }
      },
      d.value);
}

bool DataPool::less_data(data_id_t a, data_id_t b) const {
  if (a == b)
    return false;
  const auto &da = id_to_data_[a];
  const auto &db = id_to_data_[b];
  if (da.index() != db.index())
    return da.index() < db.index();
  return std::visit(
      [&](const auto &va) -> bool {
        using T = std::decay_t<decltype(va)>;
        const auto &vb = std::get<T>(db);
        if constexpr (std::is_same_v<T, std::monostate>)
          return false;
        else if constexpr (std::is_same_v<T, InternalMap>) {
          return std::lexicographical_compare(
              va.fields.begin(), va.fields.end(), vb.fields.begin(),
              vb.fields.end(), [this](const auto &x, const auto &y) {
                if (x.first != y.first)
                  return less_data(x.first, y.first);
                return less_data(x.second, y.second);
              });
        } else if constexpr (std::is_same_v<T, InternalArray>) {
          return std::lexicographical_compare(
              va.elements.begin(), va.elements.end(), vb.elements.begin(),
              vb.elements.end(),
              [this](data_id_t x, data_id_t y) { return less_data(x, y); });
        } else
          return va < vb;
      },
      da);
}

bool DataPool::columnar_rows(const InternalMap &m, size_t &rows) const {
  if (m.fields.empty())
    return false;
  bool first = true;
  for (const auto &kv : m.fields) {
    const auto *array = std::get_if<InternalArray>(&id_to_data_[kv.second]);
    if (!array)
      return false;
    if (first) {
      rows = array->elements.size();
      first = false;
    } else if (array->elements.size() != rows)
      return false;
  }
  return true;
}

data_id_t DataPool::merge_columnar(const InternalMap &ma, size_t rows_a,
                                   const InternalMap &mb, size_t rows_b) {
  const size_t columns = ma.fields.size();
  // Sort by the configured key column first, then by the other columns.
  std::vector<size_t> column_order;
  column_order.reserve(columns);
  for (size_t c = 0; c < columns; ++c) {
    if (std::get<std::string_view>(id_to_data_[ma.fields[c].first]) ==
        row_sort_key_) {
      column_order.push_back(c);
    }
  }
  for (size_t c = 0; c < columns; ++c) {
    if (column_order.empty() || column_order.front() != c)
      column_order.push_back(c);
  }

  using Row = std::vector<data_id_t>;
  std::vector<Row> rows;
  rows.reserve(rows_a + rows_b);
  auto append_rows = [&](const InternalMap &m, size_t count) {
    for (size_t r = 0; r < count; ++r) {
      Row row(columns);
      for (size_t c = 0; c < columns; ++c) {
        row[c] = std::get<InternalArray>(id_to_data_[m.fields[c].second])
                     .elements[r];
      }
      rows.push_back(std::move(row));
    }
  };
  append_rows(ma, rows_a);
  append_rows(mb, rows_b);

  auto row_less = [&](const Row &x, const Row &y) {
    for (size_t c : column_order) {
      if (x[c] != y[c])
        return less_data(x[c], y[c]);
    }
    return false;
  };
  std::sort(rows.begin(), rows.end(), row_less);
  rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

  InternalMap merged;
  merged.fields.reserve(columns);
  for (size_t c = 0; c < columns; ++c) {
    InternalArray column;
    column.elements.reserve(rows.size());
    for (const auto &row : rows)
      column.elements.push_back(row[c]);
    merged.fields.push_back({ma.fields[c].first, intern(column)});
  }
  return intern(merged);
}

data_id_t DataPool::deep_merge(data_id_t a, data_id_t b) {
  if (a == 0)
    return b;
  if (b == 0)
    return a;
  if (a == b)
    return a;

  const auto &da = id_to_data_[a];
  const auto &db = id_to_data_[b];

  if (std::holds_alternative<InternalArray>(da) &&
      std::holds_alternative<InternalArray>(db)) {
    InternalArray merged = std::get<InternalArray>(da);
    const auto &tail = std::get<InternalArray>(db).elements;
    merged.elements.insert(merged.elements.end(), tail.begin(), tail.end());
    return intern(merged);
  }

  if (std::holds_alternative<InternalMap>(da) &&
      std::holds_alternative<InternalMap>(db)) {
    // Copies: intern() may grow id_to_data_ and invalidate references.
    const InternalMap ma = std::get<InternalMap>(da);
    const InternalMap mb = std::get<InternalMap>(db);

    size_t rows_a = 0, rows_b = 0;
    bool same_keys = ma.fields.size() == mb.fields.size();
    for (size_t c = 0; same_keys && c < ma.fields.size(); ++c) {
      same_keys = ma.fields[c].first == mb.fields[c].first;
    }
    if (same_keys && columnar_rows(ma, rows_a) && columnar_rows(mb, rows_b)) {
      return merge_columnar(ma, rows_a, mb, rows_b);
    }

    InternalMap merged;
    merged.fields.reserve(ma.fields.size() + mb.fields.size());

    auto it_a = ma.fields.begin();
    auto it_b = mb.fields.begin();
    while (it_a != ma.fields.end() && it_b != mb.fields.end()) {
      if (it_a->first == it_b->first) {
        merged.fields.push_back(
            {it_a->first, deep_merge(it_a->second, it_b->second)});
        ++it_a;
        ++it_b;
      } else if (std::get<std::string_view>(id_to_data_[it_a->first]) <
                 std::get<std::string_view>(id_to_data_[it_b->first])) {
        merged.fields.push_back(*it_a);
        ++it_a;
      } else {
        merged.fields.push_back(*it_b);
        ++it_b;
      }
    }
    while (it_a != ma.fields.end())
      merged.fields.push_back(*it_a++);
    while (it_b != mb.fields.end())
      merged.fields.push_back(*it_b++);

    return intern(merged);
  }
  return b;
}

Builder::Builder(uint32_t ip_version, std::string db_type)
    : ip_version_(ip_version), database_type_(std::move(db_type)),
      build_epoch_(std::time(nullptr)) {
  // Reserve to avoid reallocations which are catastrophic for large trees
  nodes_.reserve(4000000);
  alloc_node();
}

uint32_t Builder::alloc_node() {
  uint32_t idx = nodes_.size();
  nodes_.push_back(TrieNode{});
  return idx;
}

data_id_t Builder::store_data(const Data &data) {
  std::lock_guard<std::mutex> lock(pool_mutex_);
  return pool_.store(data);
}

bool Builder::parse_network(std::string_view cidr, data_id_t data,
                            NetworkEntry &out) const noexcept {
  const auto slash = cidr.find('/');
  const std::string_view ip =
      slash == std::string_view::npos ? cidr : cidr.substr(0, slash);

  std::optional<unsigned> explicit_prefix;
  if (slash != std::string_view::npos) {
    const std::string_view text = cidr.substr(slash + 1);
    unsigned value = 0;
    if (text.empty() || text.size() > 3)
      return false;
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || ptr != text.data() + text.size())
      return false;
    explicit_prefix = value;
  }

  out.bits.fill(0);
  std::array<uint8_t, 4> v4;
  if (spoe::net::parse_ipv4(ip, v4)) {
    const unsigned prefix = explicit_prefix.value_or(32);
    if (prefix > 32)
      return false;
    // IPv4 lives at ::a.b.c.d in an IPv6 tree (libmaxminddb convention).
    const size_t offset = ip_version_ == 6 ? 12 : 0;
    std::copy(v4.begin(), v4.end(), out.bits.begin() + offset);
    out.prefix_len =
        static_cast<uint8_t>(ip_version_ == 6 ? prefix + 96 : prefix);
  } else {
    if (ip_version_ == 4)
      return false;
    if (!spoe::net::parse_ipv6(ip, out.bits))
      return false;
    const unsigned prefix = explicit_prefix.value_or(128);
    if (prefix > 128)
      return false;
    out.prefix_len = static_cast<uint8_t>(prefix);
  }
  out.data = data;
  return true;
}

void Builder::insert_parsed(std::span<const NetworkEntry> entries) {
  if (entries.empty())
    return;
  std::lock_guard<std::mutex> lock(global_mutex_);
  for (const auto &entry : entries) {
    insert_bits(entry.bits.data(), entry.prefix_len, entry.data);
  }
}

bool Builder::insert_network(std::string_view cidr, const Data &data) {
  return insert_network(cidr, store_data(data));
}

bool Builder::insert_network(std::string_view cidr, data_id_t did) {
  NetworkEntry entry;
  if (!parse_network(cidr, did, entry))
    return false;
  insert_parsed(std::span<const NetworkEntry>(&entry, 1));
  return true;
}

uint32_t Builder::descend(uint32_t from, uint8_t from_depth,
                          const uint8_t *bits, uint8_t to_depth) {
  uint32_t current = from;
  for (uint8_t depth = from_depth; depth < to_depth; ++depth) {
    const uint8_t bit = (bits[depth / 8] >> (7 - (depth % 8))) & 1;

    if (nodes_[current].data != 0) {
      uint32_t d = nodes_[current].data;
      nodes_[current].data = 0;

      nodes_[current].children[0] = alloc_node();
      const uint32_t c0 = nodes_[current].children[0];
      nodes_[c0].data = d;

      nodes_[current].children[1] = alloc_node();
      const uint32_t c1 = nodes_[current].children[1];
      nodes_[c1].data = d;
    }

    if (nodes_[current].children[bit] == 0) {
      nodes_[current].children[bit] = alloc_node();
    }

    current = nodes_[current].children[bit];
  }
  return current;
}

void Builder::insert_bits(const uint8_t *bits, uint8_t prefix_len,
                          data_id_t data_id) {
  uint32_t start = 0;
  uint8_t start_depth = 0;
  // Every IPv4 network of an IPv6 tree sits below ::/96. Internal nodes never
  // carry data (descend() splits a node before giving it children), so the
  // 96 shared hops can be skipped once that node exists.
  static constexpr uint8_t kV4Offset = 96;
  static constexpr std::array<uint8_t, 16> kZeroBits{};
  if (ip_version_ == 6 && prefix_len >= kV4Offset &&
      std::equal(bits, bits + kV4Offset / 8, kZeroBits.begin())) {
    if (ipv4_root_ == 0)
      ipv4_root_ = descend(0, 0, kZeroBits.data(), kV4Offset);
    start = ipv4_root_;
    start_depth = kV4Offset;
  }
  merge_subtree(descend(start, start_depth, bits, prefix_len), data_id);
}

void Builder::merge_subtree(uint32_t node_idx, data_id_t new_data) {
  if (nodes_[node_idx].children[0] == 0 && nodes_[node_idx].children[1] == 0) {
    const data_id_t existing = nodes_[node_idx].data;
    if (existing == 0 || existing == new_data) {
      // Most leaves are fresh: no pool access, so no pool lock.
      nodes_[node_idx].data = new_data;
    } else {
      // deep_merge interns new records; serialize with store_data().
      std::lock_guard<std::mutex> lock(pool_mutex_);
      nodes_[node_idx].data = pool_.deep_merge(existing, new_data);
    }
  } else {
    for (int i = 0; i < 2; ++i) {
      if (nodes_[node_idx].children[i] == 0) {
        nodes_[node_idx].children[i] = alloc_node();
        const uint32_t child_idx = nodes_[node_idx].children[i];
        nodes_[child_idx].data = new_data;
      } else {
        merge_subtree(nodes_[node_idx].children[i], new_data);
      }
    }
  }
}

data_id_t Builder::prune_tree_parallel(uint32_t node_idx, int depth_limit) {
  if (nodes_[node_idx].children[0] == 0 && nodes_[node_idx].children[1] == 0) {
    return nodes_[node_idx].data;
  }

  data_id_t d0 = 0, d1 = 0;
  if (depth_limit > 0) {
    std::future<data_id_t> f0, f1;
    if (nodes_[node_idx].children[0] != 0) {
      f0 = std::async(std::launch::async, &Builder::prune_tree_parallel, this,
                      nodes_[node_idx].children[0], depth_limit - 1);
    }
    if (nodes_[node_idx].children[1] != 0) {
      f1 = std::async(std::launch::async, &Builder::prune_tree_parallel, this,
                      nodes_[node_idx].children[1], depth_limit - 1);
    }
    if (f0.valid())
      d0 = f0.get();
    if (f1.valid())
      d1 = f1.get();
  } else {
    if (nodes_[node_idx].children[0] != 0)
      d0 = prune_tree_parallel(nodes_[node_idx].children[0], 0);
    if (nodes_[node_idx].children[1] != 0)
      d1 = prune_tree_parallel(nodes_[node_idx].children[1], 0);
  }

  if (d0 != 0 && d1 != 0 && d0 == d1) {
    nodes_[node_idx].children[0] = 0;
    nodes_[node_idx].children[1] = 0;
    nodes_[node_idx].data = d0;
    return d0;
  }
  return 0;
}

uint32_t Builder::assign_node_ids(uint32_t current, uint32_t &next_id,
                                  std::vector<uint32_t> &mapping,
                                  std::vector<uint32_t> &ordered) {
  if (nodes_[current].children[0] == 0 && nodes_[current].children[1] == 0)
    return 0xFFFFFFFF;
  uint32_t my_id = next_id++;
  mapping[current] = my_id;
  ordered.push_back(current);
  if (nodes_[current].children[0] != 0)
    assign_node_ids(nodes_[current].children[0], next_id, mapping, ordered);
  if (nodes_[current].children[1] != 0)
    assign_node_ids(nodes_[current].children[1], next_id, mapping, ordered);
  return my_id;
}

void Builder::collect_used_data_parallel(
    uint32_t current, int depth_limit,
    std::unordered_set<data_id_t> &out_used) {
  if (nodes_[current].children[0] == 0 && nodes_[current].children[1] == 0) {
    // Each async branch fills its own set; no shared state is touched here.
    if (nodes_[current].data != 0)
      out_used.insert(nodes_[current].data);
    return;
  }

  if (depth_limit > 0) {
    std::future<std::unordered_set<data_id_t>> f0, f1;
    if (nodes_[current].children[0] != 0) {
      f0 = std::async(std::launch::async,
                      [this, c = nodes_[current].children[0], depth_limit]() {
                        std::unordered_set<data_id_t> sub;
                        collect_used_data_parallel(c, depth_limit - 1, sub);
                        return sub;
                      });
    }
    if (nodes_[current].children[1] != 0) {
      f1 = std::async(std::launch::async,
                      [this, c = nodes_[current].children[1], depth_limit]() {
                        std::unordered_set<data_id_t> sub;
                        collect_used_data_parallel(c, depth_limit - 1, sub);
                        return sub;
                      });
    }

    if (f0.valid()) {
      auto s0 = f0.get();
      out_used.insert(s0.begin(), s0.end());
    }
    if (f1.valid()) {
      auto s1 = f1.get();
      out_used.insert(s1.begin(), s1.end());
    }
  } else {
    if (nodes_[current].children[0] != 0)
      collect_used_data_parallel(nodes_[current].children[0], 0, out_used);
    if (nodes_[current].children[1] != 0)
      collect_used_data_parallel(nodes_[current].children[1], 0, out_used);
  }
}

void Builder::encode_ctrl_and_size(std::vector<uint8_t> &out, uint8_t type,
                                   uint32_t size) {
  uint8_t ctrl_type = type < 8 ? (type << 5) : 0;
  if (size < 29) {
    out.push_back(ctrl_type | size);
  } else if (size < 285) {
    out.push_back(ctrl_type | 29);
    out.push_back(size - 29);
  } else if (size < 65821) {
    out.push_back(ctrl_type | 30);
    out.push_back((size - 285) >> 8);
    out.push_back((size - 285) & 0xFF);
  } else {
    out.push_back(ctrl_type | 31);
    uint32_t rem = size - 65821;
    out.push_back((rem >> 16) & 0xFF);
    out.push_back((rem >> 8) & 0xFF);
    out.push_back(rem & 0xFF);
  }
  if (type >= 8)
    out.push_back(type - 7);
}

void Builder::encode_pointer(std::vector<uint8_t> &out, uint32_t offset) {
  if (offset < 2048) {
    out.push_back(0x20 | ((offset >> 8) & 0x07));
    out.push_back(offset & 0xFF);
  } else if (offset < 526336) {
    offset -= 2048;
    out.push_back(0x28 | ((offset >> 16) & 0x07));
    out.push_back((offset >> 8) & 0xFF);
    out.push_back(offset & 0xFF);
  } else if (offset < 134744064) {
    offset -= 526336;
    out.push_back(0x30 | ((offset >> 24) & 0x07));
    out.push_back((offset >> 16) & 0xFF);
    out.push_back((offset >> 8) & 0xFF);
    out.push_back(offset & 0xFF);
  } else {
    out.push_back(0x38);
    out.push_back((offset >> 24) & 0xFF);
    out.push_back((offset >> 16) & 0xFF);
    out.push_back((offset >> 8) & 0xFF);
    out.push_back(offset & 0xFF);
  }
}

void Builder::encode_uint(std::vector<uint8_t> &out, uint8_t type,
                          uint64_t val) {
  if (val == 0) {
    encode_ctrl_and_size(out, type, 0);
    return;
  }
  std::vector<uint8_t> bytes;
  while (val > 0) {
    bytes.push_back(val & 0xFF);
    val >>= 8;
  }
  encode_ctrl_and_size(out, type, bytes.size());
  for (auto it = bytes.rbegin(); it != bytes.rend(); ++it)
    out.push_back(*it);
}

void Builder::encode_int(std::vector<uint8_t> &out, int32_t val) {
  encode_ctrl_and_size(out, 8, 4);
  out.push_back((val >> 24) & 0xFF);
  out.push_back((val >> 16) & 0xFF);
  out.push_back((val >> 8) & 0xFF);
  out.push_back(val & 0xFF);
}

void Builder::serialize_data(data_id_t id, std::vector<uint8_t> &out) {
  if (id == 0)
    return;
  if (auto it = emitted_.find(id); it != emitted_.end()) {
    encode_pointer(out, it->second);
    return;
  }

  const auto &d = pool_.id_to_data_[id];
  bool should_mem = std::holds_alternative<std::string_view>(d) ||
                    std::holds_alternative<InternalMap>(d) ||
                    std::holds_alternative<InternalArray>(d);

  if (should_mem)
    emitted_[id] = out.size();

  std::visit(
      [this, &out](auto &&arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
        } else if constexpr (std::is_same_v<T, std::string_view>) {
          encode_ctrl_and_size(out, 2, arg.size());
          out.insert(out.end(), arg.begin(), arg.end());
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
          encode_ctrl_and_size(out, 4, arg.size());
          out.insert(out.end(), arg.begin(), arg.end());
        } else if constexpr (std::is_same_v<T, uint16_t>) {
          encode_uint(out, 5, arg);
        } else if constexpr (std::is_same_v<T, uint32_t>) {
          encode_uint(out, 6, arg);
        } else if constexpr (std::is_same_v<T, uint64_t>) {
          encode_uint(out, 9, arg);
        } else if constexpr (std::is_same_v<T, int32_t>) {
          encode_int(out, arg);
        } else if constexpr (std::is_same_v<T, bool>) {
          encode_ctrl_and_size(out, 14, arg ? 1 : 0);
        } else if constexpr (std::is_same_v<T, double>) {
          encode_ctrl_and_size(out, 3, 8);
          uint64_t v;
          std::memcpy(&v, &arg, 8);
          for (int i = 7; i >= 0; --i)
            out.push_back((v >> (i * 8)) & 0xFF);
        } else if constexpr (std::is_same_v<T, float>) {
          encode_ctrl_and_size(out, 15, 4);
          uint32_t v;
          std::memcpy(&v, &arg, 4);
          for (int i = 3; i >= 0; --i)
            out.push_back((v >> (i * 8)) & 0xFF);
        } else if constexpr (std::is_same_v<T, InternalMap>) {
          encode_ctrl_and_size(out, 7, arg.fields.size());
          for (const auto &kv : arg.fields) {
            serialize_data(kv.first, out);
            serialize_data(kv.second, out);
          }
        } else if constexpr (std::is_same_v<T, InternalArray>) {
          encode_ctrl_and_size(out, 11, arg.elements.size());
          for (auto item : arg.elements)
            serialize_data(item, out);
        }
      },
      d);
}

void Builder::write_metadata(std::vector<uint8_t> &out, uint32_t node_count) {
  std::string_view marker = "\xAB\xCD\xEFMaxMind.com";
  out.insert(out.end(), marker.begin(), marker.end());

  Object desc;
  desc.insert({"en", Data("Generated Database")});

  Array langs;
  langs.push_back(Data("en"));

  Object meta;
  meta.insert({"node_count", Data(node_count)});
  meta.insert({"record_size", Data(uint16_t(28))});
  meta.insert({"ip_version", Data(uint16_t(ip_version_))});
  meta.insert({"database_type", Data(database_type_)});
  meta.insert({"languages", Data(langs)});
  meta.insert({"binary_format_major_version", Data(uint16_t(2))});
  meta.insert({"binary_format_minor_version", Data(uint16_t(0))});
  meta.insert({"build_epoch", Data(uint64_t(build_epoch_))});
  meta.insert({"description", Data(desc)});

  emitted_.clear();
  data_id_t meta_id = pool_.store(Data(meta));
  serialize_data(meta_id, out);
}

BuildStats Builder::build(const std::string &output_filename) {
  prune_tree_parallel(0, 4);

  std::vector<uint32_t> old_to_new(nodes_.size(), 0xFFFFFFFF);
  std::vector<uint32_t> ordered_nodes;
  ordered_nodes.reserve(nodes_.size());
  uint32_t node_count = 0;
  assign_node_ids(0, node_count, old_to_new, ordered_nodes);

  std::unordered_set<data_id_t> used;
  collect_used_data_parallel(0, 4, used);

  std::vector<uint8_t> data_section;
  data_section.reserve(used.size() * 32);
  data_section.push_back(0);

  emitted_.clear();
  std::vector<uint32_t> data_offsets(pool_.id_to_data_.size(), 0);

  // Serialize in content order: ids depend on the order in which merged
  // records were interned, which varies with thread timing, whereas content
  // does not. This makes the output reproducible run to run.
  std::vector<data_id_t> ordered_ids(used.begin(), used.end());
  std::sort(ordered_ids.begin(), ordered_ids.end(),
            [this](data_id_t a, data_id_t b) { return pool_.less_data(a, b); });
  for (data_id_t id : ordered_ids) {
    if (id == 0)
      continue;
    if (const auto emitted = emitted_.find(id); emitted != emitted_.end()) {
      data_offsets[id] = emitted->second;
    } else {
      data_offsets[id] = data_section.size();
      serialize_data(id, data_section);
    }
  }

  std::ofstream os(output_filename, std::ios::binary | std::ios::trunc);
  if (!os)
    throw std::runtime_error("Could not open file for writing");
  os.exceptions(std::ios::badbit | std::ios::failbit);

  constexpr size_t BUFFER_SIZE = 1048576;
  std::vector<uint8_t> buffer;
  buffer.reserve(BUFFER_SIZE);

  for (uint32_t current : ordered_nodes) {
    auto resolve = [&](uint32_t child_idx) -> uint32_t {
      if (child_idx == 0)
        return node_count;
      uint32_t cid = old_to_new[child_idx];
      if (cid != 0xFFFFFFFF)
        return cid;
      data_id_t did = nodes_[child_idx].data;
      if (did == 0)
        return node_count;
      return node_count + 16 + data_offsets[did];
    };

    uint32_t rec_left = resolve(nodes_[current].children[0]);
    uint32_t rec_right = resolve(nodes_[current].children[1]);

    buffer.push_back((rec_left >> 16) & 0xFF);
    buffer.push_back((rec_left >> 8) & 0xFF);
    buffer.push_back(rec_left & 0xFF);
    buffer.push_back((((rec_left >> 24) & 0x0F) << 4) |
                     ((rec_right >> 24) & 0x0F));
    buffer.push_back((rec_right >> 16) & 0xFF);
    buffer.push_back((rec_right >> 8) & 0xFF);
    buffer.push_back(rec_right & 0xFF);

    if (buffer.size() >= BUFFER_SIZE - 7) {
      os.write(reinterpret_cast<const char *>(buffer.data()), buffer.size());
      buffer.clear();
    }
  }

  if (!buffer.empty()) {
    os.write(reinterpret_cast<const char *>(buffer.data()), buffer.size());
  }

  const std::array<char, 16> separator{};
  os.write(separator.data(), separator.size());

  os.write(reinterpret_cast<const char *>(data_section.data()),
           data_section.size());

  std::vector<uint8_t> meta_section;
  write_metadata(meta_section, node_count);
  os.write(reinterpret_cast<const char *>(meta_section.data()),
           meta_section.size());
  os.flush();
  os.close();

  BuildStats stats;
  stats.node_count = node_count;
  stats.data_records = ordered_ids.size();
  stats.data_section_bytes = data_section.size();
  stats.total_bytes = static_cast<size_t>(node_count) * 7 + separator.size() +
                      data_section.size() + meta_section.size();
  return stats;
}

} // namespace mmdb_builder
