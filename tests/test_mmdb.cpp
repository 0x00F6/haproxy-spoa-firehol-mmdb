#include "mmdb/mmdb.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

// Tiny data-section fixtures exercise the real decoder without a database file.
// Short strings use type 2; arrays use extended type 11, maps use type 7.
std::string string_value(std::string_view value) {
  require(value.size() < 29, "fixture string too long");
  return std::string(1, static_cast<char>(0x40 | value.size())).append(value);
}

std::string array_value(const std::vector<std::string> &values) {
  require(values.size() < 29, "fixture array too long");
  std::string encoded{static_cast<char>(values.size()), '\x04'};
  for (const auto &value : values)
    encoded += value;
  return encoded;
}

std::string category_map(const std::string &value) {
  return "\xe1" + string_value("category") + value;
}

struct Entry {
  std::string encoded;
  MMDB_s database{};
  MMDB_lookup_result_s result{};

  explicit Entry(std::string data) : encoded(std::move(data)) {
    database.data_section = reinterpret_cast<const uint8_t *>(encoded.data());
    database.data_section_size = static_cast<uint32_t>(encoded.size());
    result.found_entry = true;
    result.entry = {&database, 0};
  }
};

void test_categories() {
  Entry mixed(category_map(
      array_value({string_value("abuse"), std::string("\xa1\x07", 2),
                   array_value({string_value("ignored")}),
                   string_value("unroutable"), string_value("abuse")})));
  const std::vector<std::string_view> expected{"abuse", "unroutable", "abuse"};
  const auto categories = spoe::Mmdb::get_categories(mixed.result);
  require(categories == expected,
          "category order, duplicates or filtering changed");
  require(categories.front().data() == mixed.encoded.data() + 13,
          "category strings no longer borrow database storage");
  require(
      spoe::Mmdb::format_entry(mixed.result) ==
          "{\"category\":[\"abuse\",7,[\"ignored\"],\"unroutable\",\"abuse\"]}",
      "nested entry formatting changed");

  // The map points to an array at offset 12. Its second member points back to
  // the first string at offset 14, as real databases do for repeated values.
  Entry pointers(category_map(std::string("\x20\x0c", 2)) +
                 array_value({string_value("abuse"), std::string("\x20\x0e", 2),
                              string_value("unroutable")}));
  require(spoe::Mmdb::get_categories(pointers.result) ==
              std::vector<std::string_view>{"abuse", "abuse", "unroutable"},
          "category array or string pointers were not resolved");
  require(spoe::Mmdb::format_entry(pointers.result) ==
              "{\"category\":[\"abuse\",\"abuse\",\"unroutable\"]}",
          "pointer entry formatting changed");

  Entry missing("\xe0");
  Entry empty(category_map(array_value({})));
  Entry scalar(category_map(string_value("abuse")));
  require(spoe::Mmdb::get_categories(missing.result).empty(),
          "missing category accepted");
  require(spoe::Mmdb::get_categories(empty.result).empty(),
          "empty category array changed");
  require(spoe::Mmdb::get_categories(scalar.result).empty(),
          "scalar category accepted");

  Entry truncated(category_map(array_value({string_value("abuse"), "\x48x"})));
  require(spoe::Mmdb::get_categories(truncated.result) ==
              std::vector<std::string_view>{"abuse"},
          "corrupt member discarded earlier valid categories");
  require(spoe::Mmdb::format_entry(truncated.result) ==
              "(failed to read entry data)",
          "corrupt entry did not report formatting failure");

  MMDB_lookup_result_s absent{};
  require(spoe::Mmdb::get_categories(absent).empty(),
          "absent entry has categories");
  require(spoe::Mmdb::format_entry(absent).empty(),
          "absent entry has formatted data");
}

void test_scalar_formatting() {
  Entry scalars(
      array_value({std::string("\x82\x00\xff", 3),         // Bytes.
                   std::string("\xa2\xff\xff", 3),         // uint16 maximum.
                   std::string("\xc4\xff\xff\xff\xff", 5), // uint32 maximum.
                   std::string("\x04\x01\xff\xff\xff\xff", 6), // int32 -1.
                   std::string("\x08\x02\xff\xff\xff\xff\xff\xff\xff\xff", 10),
                   std::string("\x01\x03\xab", 3), // uint128 in hex.
                   std::string("\x00\x03", 2),     // uint128 zero.
                   std::string("\x01\x07", 2),     // true.
                   std::string("\x00\x07", 2),     // false.
                   std::string("\x04\x08\x3f\xc0\x00\x00", 6), // float 1.5.
                   std::string("\x68\x3f\xf8\x00\x00\x00\x00\x00\x00", 9)}));
  const std::string expected =
      "[0x00ff,65535,4294967295,-1,18446744073709551615,"
#if MMDB_UINT128_IS_BYTE_ARRAY
      "000000000000000000000000000000ab,00000000000000000000000000000000,"
#else
      "ab,0,"
#endif
      "true,false,1.500000,1.500000]";
  require(spoe::Mmdb::format_entry(scalars.result) == expected,
          "scalar entry formatting changed");
}

} // namespace

int main() {
  try {
    test_categories();
    test_scalar_formatting();
    std::cout << "MMDB entry tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
