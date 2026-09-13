#pragma once

#include <cstdlib>
#include <string_view>

namespace spoe::utils {

//! Value of `name`, or nullptr when the variable is unset or empty.
[[nodiscard]] inline const char *
environment_non_empty(const char *name) noexcept {
  const char *value = std::getenv(name);
  return (value && *value) ? value : nullptr;
}

//! Value of `name`, or `fallback` when the variable is unset. An empty value
//! is returned as is so callers can distinguish "unset" from "disabled".
[[nodiscard]] inline std::string_view
environment_or(const char *name, std::string_view fallback) noexcept {
  const char *value = std::getenv(name);
  return value ? std::string_view(value) : fallback;
}

} // namespace spoe::utils
