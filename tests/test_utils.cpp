// Unit tests for the shared helpers in src/utils.
#include "utils/environment.h"
#include "utils/net_utils.h"
#include "utils/string_utils.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
  }
}

void test_trim() {
  using spoe::utils::trim;
  check(trim("  abc \t\r\n") == "abc", "trim strips both ends");
  check(trim("abc") == "abc", "trim keeps clean input");
  check(trim("   ").empty(), "trim of whitespace is empty");
  check(trim("").empty(), "trim of empty is empty");
  check(trim(" a b ") == "a b", "trim keeps inner whitespace");
  const std::string_view source = "  view  ";
  const auto view = trim(source);
  check(view.data() == source.data() + 2, "trim returns a view into the input");
}

void test_group_thousands() {
  using spoe::utils::group_thousands;
  check(group_thousands(0) == "0", "0");
  check(group_thousands(999) == "999", "999");
  check(group_thousands(1000) == "1 000", "1 000");
  check(group_thousands(12) == "12", "12");
  check(group_thousands(123456) == "123 456", "123 456");
  check(group_thousands(7856340) == "7 856 340", "7 856 340");
  check(group_thousands(17377982) == "17 377 982", "17 377 982");
  check(group_thousands(100000000) == "100 000 000", "100 000 000");
  check(group_thousands(1234567890123ULL) == "1 234 567 890 123", "13 digits");
}

void test_parse_ipv4() {
  using spoe::net::parse_ipv4;
  std::array<uint8_t, 4> octets{};
  check(parse_ipv4("1.2.3.4", octets) && octets == std::array<uint8_t, 4>{1, 2, 3, 4},
        "dotted quad");
  check(parse_ipv4("0.0.0.0", octets) && octets == std::array<uint8_t, 4>{0, 0, 0, 0},
        "all zeros");
  check(parse_ipv4("255.255.255.255", octets) &&
            octets == std::array<uint8_t, 4>{255, 255, 255, 255},
        "all 255");
  // Same rejections as glibc inet_pton(AF_INET).
  for (const std::string_view invalid :
       {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "01.2.3.4", "1.2.3.4 ", " 1.2.3.4",
        "+1.2.3.4", "1..2.3", ".1.2.3", "1.2.3.", "a.b.c.d", "1.2.3.4/24", "1234.1.1.1"}) {
    check(!parse_ipv4(invalid, octets), std::string("reject ") + std::string(invalid));
  }
}

void test_parse_ipv6() {
  using spoe::net::parse_ipv6;
  std::array<uint8_t, 16> octets{};
  check(parse_ipv6("::1", octets) && octets[15] == 1, "loopback");
  check(parse_ipv6("2001:db8::ff00:42:8329", octets) && octets[0] == 0x20 && octets[1] == 0x01,
        "documentation prefix");
  check(!parse_ipv6("", octets), "reject empty");
  check(!parse_ipv6("1.2.3.4", octets), "reject IPv4 text");
  check(!parse_ipv6(std::string(60, 'f'), octets), "reject oversized text");
}

void test_environment() {
  using spoe::utils::environment_non_empty;
  using spoe::utils::environment_or;
  unsetenv("SPOA_TEST_VARIABLE");
  check(environment_or("SPOA_TEST_VARIABLE", "fallback") == "fallback", "unset uses fallback");
  check(environment_non_empty("SPOA_TEST_VARIABLE") == nullptr, "unset is null");
  setenv("SPOA_TEST_VARIABLE", "", 1);
  check(environment_or("SPOA_TEST_VARIABLE", "fallback").empty(), "empty stays empty");
  check(environment_non_empty("SPOA_TEST_VARIABLE") == nullptr, "empty is null");
  setenv("SPOA_TEST_VARIABLE", "value", 1);
  check(environment_or("SPOA_TEST_VARIABLE", "fallback") == "value", "set value wins");
  check(std::string_view(environment_non_empty("SPOA_TEST_VARIABLE")) == "value",
        "non-empty value is returned");
  unsetenv("SPOA_TEST_VARIABLE");
}

} // namespace

int main() {
  test_trim();
  test_group_thousands();
  test_parse_ipv4();
  test_parse_ipv6();
  test_environment();
  if (failures == 0) {
    std::cout << "All utils tests passed!\n";
    return 0;
  }
  return 1;
}
