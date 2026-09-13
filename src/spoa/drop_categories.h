#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spoe {

//! Transparent hash enabling heterogeneous lookup of a container keyed by
//! std::string using a std::string_view (zero-copy, no temporary allocation).
struct TransparentHash {
  using is_transparent = void;
  size_t operator()(std::string_view v) const noexcept {
    return std::hash<std::string_view>{}(v);
  }
};

//! Configured categories own both their names and blocked counters.
//! Finish loading before registering metrics or processing requests. After
//! configuration, only the atomic counters change; returned name views stay
//! valid until this object is destroyed.
class DropCategories {
public:
  //! Parse a comma-separated category list (e.g. "unroutable,abuse").
  void load(std::string_view raw);

  [[nodiscard]] size_t size() const noexcept { return categories_.size(); }
  [[nodiscard]] bool empty() const noexcept { return categories_.empty(); }
  //! Configured names in sorted order.
  [[nodiscard]] std::vector<std::string_view> names() const;
  //! Sorted names joined with ", ", for logging.
  [[nodiscard]] std::string names_string() const;

  //! Increment the counter of `category` if configured and return its owned
  //! name, or an empty view otherwise.
  std::string_view match_and_increment(std::string_view category) noexcept;

  //! Count only the first configured match in database order. Combining the
  //! decision and increment avoids looking up the same category twice.
  std::string_view
  match_and_increment(std::span<const std::string_view> categories) noexcept;

  //! Current counter value for `key` (0 if unknown).
  [[nodiscard]] uint64_t blocked(std::string_view key) const noexcept;

private:
  std::unordered_map<std::string, std::atomic<uint64_t>, TransparentHash,
                     std::equal_to<>>
      categories_;
};

} // namespace spoe
