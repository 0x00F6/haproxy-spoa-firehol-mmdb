#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace spoe::utils {

inline constexpr std::string_view kWhitespace = " \t\r\n";

//! Returns `text` without leading and trailing whitespace. The view borrows
//! the input and must not outlive it.
[[nodiscard]] constexpr std::string_view trim(std::string_view text) noexcept {
  const size_t begin = text.find_first_not_of(kWhitespace);
  if (begin == std::string_view::npos) {
    return {};
  }
  return text.substr(begin, text.find_last_not_of(kWhitespace) - begin + 1);
}

//! Groups digits by three with a space: 7856340 -> "7 856 340".
[[nodiscard]] inline std::string group_thousands(uint64_t value) {
  const std::string digits = std::to_string(value);
  std::string out;
  out.reserve(digits.size() + digits.size() / 3);
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i != 0 && (digits.size() - i) % 3 == 0) {
      out.push_back(' ');
    }
    out.push_back(digits[i]);
  }
  return out;
}

} // namespace spoe::utils
