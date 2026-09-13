#include "spoa/drop_categories.h"
#include "utils/string_utils.h"

#include <algorithm>
#include <ranges>

namespace spoe {

void DropCategories::load(std::string_view config) {
  categories_.clear();
  for (const auto token : std::views::split(config, ',')) {
    const auto name = utils::trim(std::string_view(token.begin(), token.end()));
    if (!name.empty()) {
      categories_.try_emplace(std::string(name), 0);
    }
  }
}

std::vector<std::string_view> DropCategories::names() const {
  std::vector<std::string_view> names;
  names.reserve(categories_.size());
  for (const auto &[name, _] : categories_) {
    names.push_back(name);
  }
  std::ranges::sort(names);
  return names;
}

std::string DropCategories::names_string() const {
  const auto category_names = names();
  size_t estimated_len = 0;
  for (const auto name : category_names) {
    estimated_len += name.size() + 2;
  }
  std::string out;
  out.reserve(estimated_len);
  for (const auto name : category_names) {
    if (!out.empty()) {
      out += ", ";
    }
    out += name;
  }
  return out;
}

std::string_view
DropCategories::match_and_increment(std::string_view category) noexcept {
  const auto it = categories_.find(category);
  if (it == categories_.end()) {
    return {};
  }
  it->second.fetch_add(1, std::memory_order_relaxed);
  return it->first;
}

std::string_view DropCategories::match_and_increment(
    std::span<const std::string_view> categories) noexcept {
  for (const auto category : categories) {
    if (const auto matched = match_and_increment(category); !matched.empty()) {
      return matched;
    }
  }
  return {};
}

uint64_t DropCategories::blocked(std::string_view name) const noexcept {
  const auto it = categories_.find(name);
  return it == categories_.end() ? 0
                                 : it->second.load(std::memory_order_relaxed);
}

} // namespace spoe
