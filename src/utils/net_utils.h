#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace spoe::net {

//! Stack-allocated text representation of an IP address.
struct AddressText {
  char text[INET6_ADDRSTRLEN] = "<invalid address>";
};

//! Format IPv4 or IPv6 binary octets into presentation text without heap
//! allocation.
template <size_t N>
[[nodiscard]] inline AddressText
format_ip(const std::array<uint8_t, N> &octets) noexcept {
  static_assert(N == 4 || N == 16);
  AddressText out;
  if (!inet_ntop(N == 4 ? AF_INET : AF_INET6, octets.data(), out.text,
                 sizeof(out.text))) {
    out.text[0] = '\0';
  }
  return out;
}

//! Parse a dotted-quad IPv4 address without copying or calling libc. Accepts
//! exactly what glibc's inet_pton(AF_INET) accepts: four decimal octets in
//! 0..255, no leading zeros, no surrounding characters.
[[nodiscard]] constexpr bool
parse_ipv4(std::string_view text, std::array<uint8_t, 4> &octets) noexcept {
  size_t pos = 0;
  for (size_t octet = 0; octet < 4; ++octet) {
    unsigned value = 0;
    size_t digits = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
      if (digits == 1 && value == 0) {
        return false; // leading zero
      }
      value = value * 10 + static_cast<unsigned>(text[pos] - '0');
      if (value > 255 || ++digits > 3) {
        return false;
      }
      ++pos;
    }
    if (digits == 0) {
      return false;
    }
    octets[octet] = static_cast<uint8_t>(value);
    if (octet < 3) {
      if (pos >= text.size() || text[pos] != '.') {
        return false;
      }
      ++pos;
    }
  }
  return pos == text.size();
}

//! Parse an IPv6 presentation string without heap allocation.
//! Returns true and fills `octets` on success.
[[nodiscard]] inline bool parse_ipv6(std::string_view text,
                                     std::array<uint8_t, 16> &octets) noexcept {
  if (text.empty() || text.size() >= INET6_ADDRSTRLEN) {
    return false;
  }
  char buf[INET6_ADDRSTRLEN];
  std::memcpy(buf, text.data(), text.size());
  buf[text.size()] = '\0';
  return ::inet_pton(AF_INET6, buf, octets.data()) == 1;
}

} // namespace spoe::net
