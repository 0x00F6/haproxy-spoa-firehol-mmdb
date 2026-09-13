#pragma once

#include <maxminddb.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spoe {

// Owns a mapped MaxMind database. Moving transfers ownership of the mapping.
class Mmdb {
public:
  Mmdb() = default;
  explicit Mmdb(const std::string &path);
  ~Mmdb();

  Mmdb(const Mmdb &) = delete;
  Mmdb &operator=(const Mmdb &) = delete;
  Mmdb(Mmdb &&other) noexcept;
  Mmdb &operator=(Mmdb &&other) noexcept;

  // Reopening closes the previous database; failures throw std::runtime_error.
  void open(const std::string &path);
  void close();

  bool is_open() const { return open_; }
  const std::string &path() const { return path_; }

  // Compact log metadata with the build time expressed in UTC.
  std::string metadata_string() const;

  uint64_t build_epoch() const { return mmdb_.metadata.build_epoch; }

  // Binary addresses use network byte order. A missing entry is not an error;
  // mmdb_error receives MMDB_SUCCESS unless the lookup itself fails.
  // Returned entries borrow the database mapping and must not outlive it.
  MMDB_lookup_result_s lookup_ipv4(const std::array<uint8_t, 4> &octets,
                                   int *mmdb_error = nullptr) const;
  MMDB_lookup_result_s lookup_ipv6(const std::array<uint8_t, 16> &octets,
                                   int *mmdb_error = nullptr) const;
  MMDB_lookup_result_s lookup_string(std::string_view ipstr,
                                     int *mmdb_error = nullptr) const;

  // Formats a found entry as compact JSON-like text; missing entries are empty.
  static std::string format_entry(MMDB_lookup_result_s &result);

  // Calls `visit(std::string_view)` for each string member of "category" in
  // database order, duplicates included, until it returns false. Views borrow
  // the mapping; retain the database snapshot while using them. Corrupt
  // members are skipped without discarding the valid ones.
  template <typename Visit>
  static void for_each_category(MMDB_lookup_result_s &result, Visit &&visit) {
    const CategoryArray array = category_array(result);
    for (uint32_t index = 0; index < array.size; ++index) {
      const auto category = category_at(array, index);
      if (category && !visit(*category)) {
        return;
      }
    }
  }

  // Collects the categories for_each_category() would visit.
  static std::vector<std::string_view>
  get_categories(MMDB_lookup_result_s &result);

private:
  // The resolved "category" array; size is 0 when absent or not an array.
  struct CategoryArray {
    MMDB_entry_s entry{};
    uint32_t size = 0;
  };
  static CategoryArray category_array(MMDB_lookup_result_s &result);
  static std::optional<std::string_view> category_at(const CategoryArray &array,
                                                     uint32_t index);

  MMDB_s mmdb_{};
  bool open_ = false;
  std::string path_;
};

} // namespace spoe
