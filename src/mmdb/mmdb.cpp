#include "mmdb/mmdb.h"
#include "utils/net_utils.h"
#include "utils/string_utils.h"

#include <charconv>
#include <cstring>
#include <ctime>
#include <format>
#include <limits>
#include <memory>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <utility>

namespace spoe {
namespace {

constexpr char hex_digits[] = "0123456789abcdef";

struct EntryDataDeleter {
  void operator()(MMDB_entry_data_list_s *list) const noexcept {
    MMDB_free_entry_data_list(list);
  }
};
using EntryDataList = std::unique_ptr<MMDB_entry_data_list_s, EntryDataDeleter>;

MMDB_lookup_result_s lookup_sockaddr(const MMDB_s &db, const sockaddr *address,
                                     int *mmdb_error) noexcept {
  int error = MMDB_SUCCESS;
  const auto result = MMDB_lookup_sockaddr(&db, address, &error);
  if (mmdb_error) {
    *mmdb_error = error;
  }
  return result;
}

std::string format_epoch(uint64_t epoch) {
  const auto time = static_cast<std::time_t>(epoch);
  std::tm utc{};
  char buffer[32]{};
  if (gmtime_r(&time, &utc)) {
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &utc);
  }
  return buffer;
}

template <typename Integer>
void append_integer(std::string &out, Integer value) {
  char buffer[std::numeric_limits<Integer>::digits10 + 3];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  out.append(buffer, result.ptr);
}

void append_hex(std::string &out, const uint8_t *bytes, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    out += hex_digits[bytes[i] >> 4];
    out += hex_digits[bytes[i] & 0x0f];
  }
}

void append_uint128(std::string &out, const MMDB_entry_data_s &data) {
#if MMDB_UINT128_IS_BYTE_ARRAY
  append_hex(out, data.uint128, 16);
#else
  auto value = data.uint128;
  char buffer[32];
  auto *begin = buffer + sizeof(buffer);
  do {
    *--begin = hex_digits[value & 0x0f];
    value >>= 4;
  } while (value != 0);
  out.append(begin, buffer + sizeof(buffer));
#endif
}

// Each call consumes one complete value, including nested array/map members.
void dump_node(std::string &out, MMDB_entry_data_list_s *&cursor) {
  if (!cursor) {
    out += "null";
    return;
  }
  const auto &data = cursor->entry_data;
  cursor = cursor->next;
  switch (data.type) {
  case MMDB_DATA_TYPE_MAP:
    out += '{';
    for (uint32_t i = 0; i < data.data_size; ++i) {
      if (i) {
        out += ',';
      }
      if (cursor && cursor->entry_data.type == MMDB_DATA_TYPE_UTF8_STRING) {
        out += '"';
        out.append(cursor->entry_data.utf8_string,
                   cursor->entry_data.data_size);
        out += "\":";
        cursor = cursor->next;
      }
      dump_node(out, cursor);
    }
    out += '}';
    break;
  case MMDB_DATA_TYPE_ARRAY:
    out += '[';
    for (uint32_t i = 0; i < data.data_size; ++i) {
      if (i) {
        out += ',';
      }
      dump_node(out, cursor);
    }
    out += ']';
    break;
  case MMDB_DATA_TYPE_UTF8_STRING:
    out += '"';
    out.append(data.utf8_string, data.data_size);
    out += '"';
    break;
  case MMDB_DATA_TYPE_DOUBLE:
    out += std::to_string(data.double_value);
    break;
  case MMDB_DATA_TYPE_FLOAT:
    out += std::to_string(data.float_value);
    break;
  case MMDB_DATA_TYPE_BOOLEAN:
    out += data.boolean ? "true" : "false";
    break;
  case MMDB_DATA_TYPE_UINT16:
    append_integer(out, data.uint16);
    break;
  case MMDB_DATA_TYPE_UINT32:
    append_integer(out, data.uint32);
    break;
  case MMDB_DATA_TYPE_INT32:
    append_integer(out, data.int32);
    break;
  case MMDB_DATA_TYPE_UINT64:
    append_integer(out, data.uint64);
    break;
  case MMDB_DATA_TYPE_BYTES:
    out += "0x";
    append_hex(out, data.bytes, data.data_size);
    break;
  case MMDB_DATA_TYPE_UINT128:
    append_uint128(out, data);
    break;
  default:
    out += "null";
    break;
  }
}

} // namespace

Mmdb::Mmdb(const std::string &path) { open(path); }

Mmdb::~Mmdb() { close(); }

Mmdb::Mmdb(Mmdb &&other) noexcept
    : mmdb_(std::exchange(other.mmdb_, {})),
      open_(std::exchange(other.open_, false)), path_(std::move(other.path_)) {}

Mmdb &Mmdb::operator=(Mmdb &&other) noexcept {
  if (this != &other) {
    close();
    mmdb_ = std::exchange(other.mmdb_, {});
    open_ = std::exchange(other.open_, false);
    path_ = std::move(other.path_);
  }
  return *this;
}

void Mmdb::open(const std::string &path) {
  // Copy before closing: path may refer to this instance's path(). Allocate
  // before acquiring a mapping so a failed constructor cannot leak it.
  std::string opened_path(path);
  close();
  MMDB_s handle{};
  const int status = MMDB_open(opened_path.c_str(), MMDB_MODE_MMAP, &handle);
  if (status != MMDB_SUCCESS) {
    throw std::runtime_error(std::format("MMDB_open failed for '{}': {}",
                                         opened_path, MMDB_strerror(status)));
  }
  mmdb_ = handle;
  open_ = true;
  path_ = std::move(opened_path);
}

void Mmdb::close() {
  if (open_) {
    MMDB_close(&mmdb_);
    mmdb_ = {};
    open_ = false;
    path_.clear();
  }
}

std::string Mmdb::metadata_string() const {
  if (!open_) {
    return "(database not open)";
  }
  const auto &metadata = mmdb_.metadata;
  std::string languages;
  for (size_t i = 0; i < metadata.languages.count; ++i) {
    if (i) {
      languages += ',';
    }
    languages +=
        metadata.languages.names[i] ? metadata.languages.names[i] : "?";
  }
  const char *description = "";
  if (metadata.description.count > 0 && metadata.description.descriptions[0] &&
      metadata.description.descriptions[0]->description) {
    description = metadata.description.descriptions[0]->description;
  }
  return std::format(
      "type={} ip_version={} record_size={} node_count={} format={}.{} "
      "built_at={} languages={} description={}",
      metadata.database_type ? metadata.database_type : "?",
      metadata.ip_version, metadata.record_size,
      utils::group_thousands(metadata.node_count),
      metadata.binary_format_major_version,
      metadata.binary_format_minor_version, format_epoch(metadata.build_epoch),
      languages, description);
}

MMDB_lookup_result_s Mmdb::lookup_ipv4(const std::array<uint8_t, 4> &octets,
                                       int *mmdb_error) const {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  std::memcpy(&address.sin_addr, octets.data(), octets.size());
  return lookup_sockaddr(mmdb_, reinterpret_cast<const sockaddr *>(&address),
                         mmdb_error);
}

MMDB_lookup_result_s Mmdb::lookup_ipv6(const std::array<uint8_t, 16> &octets,
                                       int *mmdb_error) const {
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  std::memcpy(&address.sin6_addr, octets.data(), octets.size());
  return lookup_sockaddr(mmdb_, reinterpret_cast<const sockaddr *>(&address),
                         mmdb_error);
}

MMDB_lookup_result_s Mmdb::lookup_string(std::string_view ipstr,
                                         int *mmdb_error) const {
  // Fast path: attempt zero-allocation numeric parsing before getaddrinfo.
  std::array<uint8_t, 4> v4;
  if (net::parse_ipv4(ipstr, v4)) {
    return lookup_ipv4(v4, mmdb_error);
  }
  std::array<uint8_t, 16> v6;
  if (net::parse_ipv6(ipstr, v6)) {
    return lookup_ipv6(v6, mmdb_error);
  }

  // Fallback for non-standard formats or hostname strings.
  char stack[64];
  std::string heap;
  const char *terminated = stack;
  if (ipstr.size() < sizeof(stack)) {
    std::memcpy(stack, ipstr.data(), ipstr.size());
    stack[ipstr.size()] = '\0';
  } else {
    heap.assign(ipstr);
    terminated = heap.c_str();
  }
  int gai_error = 0;
  int error = MMDB_SUCCESS;
  const auto result =
      MMDB_lookup_string(&mmdb_, terminated, &gai_error, &error);
  if (gai_error != 0) {
    error = MMDB_INVALID_DATA_ERROR - 1000 + gai_error;
  }
  if (mmdb_error) {
    *mmdb_error = error;
  }
  return result;
}

std::string Mmdb::format_entry(MMDB_lookup_result_s &result) {
  if (!result.found_entry) {
    return {};
  }
  MMDB_entry_data_list_s *head = nullptr;
  const int status = MMDB_get_entry_data_list(&result.entry, &head);
  const EntryDataList list(head);
  if (status != MMDB_SUCCESS || !list) {
    return "(failed to read entry data)";
  }
  // Traversal consumes its cursor; keep the original head to free the pool,
  // including when appending to the output throws.
  auto *cursor = list.get();
  std::string out;
  dump_node(out, cursor);
  return out;
}

Mmdb::CategoryArray Mmdb::category_array(MMDB_lookup_result_s &result) {
  if (!result.found_entry) {
    return {};
  }
  const char *path[] = {"category", nullptr};
  MMDB_entry_data_s array{};
  if (MMDB_aget_value(&result.entry, &array, path) != MMDB_SUCCESS ||
      array.type != MMDB_DATA_TYPE_ARRAY) {
    return {};
  }
  // Start at the resolved array to avoid rescanning the entry's map for every
  // category.
  return {.entry = {result.entry.mmdb, array.offset}, .size = array.data_size};
}

std::optional<std::string_view> Mmdb::category_at(const CategoryArray &array,
                                                  uint32_t index) {
  char digits[std::numeric_limits<uint32_t>::digits10 + 2];
  const auto converted =
      std::to_chars(digits, digits + sizeof(digits) - 1, index);
  *converted.ptr = '\0';
  const char *path[] = {digits, nullptr};
  MMDB_entry_s entry = array.entry;
  MMDB_entry_data_s element{};
  if (MMDB_aget_value(&entry, &element, path) != MMDB_SUCCESS ||
      element.type != MMDB_DATA_TYPE_UTF8_STRING) {
    return std::nullopt;
  }
  return std::string_view(element.utf8_string, element.data_size);
}

std::vector<std::string_view>
Mmdb::get_categories(MMDB_lookup_result_s &result) {
  std::vector<std::string_view> out;
  const CategoryArray array = category_array(result);
  out.reserve(array.size);
  for (uint32_t index = 0; index < array.size; ++index) {
    if (const auto category = category_at(array, index)) {
      out.push_back(*category);
    }
  }
  return out;
}

} // namespace spoe
