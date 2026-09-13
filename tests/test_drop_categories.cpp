#include "spoa/drop_categories.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void test_configuration_and_decisions() {
  spoe::DropCategories categories;
  {
    const std::string config = " , abuse,\tunroutable\r\n,abuse,,Abuse, ";
    categories.load(config);
  }
  require(categories.size() == 3, "blank or duplicate categories retained");
  require(categories.names_string() == "Abuse, abuse, unroutable",
          "category names must be owned, trimmed, sorted and case-sensitive");

  const std::array<std::string_view, 4> entry{"unknown", "unroutable", "abuse",
                                              "unroutable"};
  require(categories.match_and_increment(entry) == "unroutable",
          "first configured category in database order must win");
  require(categories.blocked("unroutable") == 1 &&
              categories.blocked("abuse") == 0,
          "only the first match should be counted once per lookup");

  const std::array<std::string_view, 2> misses{"ABUSE", "unknown"};
  require(categories.match_and_increment(misses).empty(),
          "matching must be exact");
  require(categories.blocked("unknown") == 0 && categories.size() == 3,
          "unknown categories must not create counters");

  spoe::DropCategories empty;
  empty.load(" ,\t,\n");
  require(empty.empty() && empty.names_string().empty() &&
              empty.match_and_increment(entry).empty(),
          "empty configuration must leave addresses unflagged");
}

void test_counter_updates() {
  spoe::DropCategories categories;
  categories.load("abuse");
  const auto increment = [&categories] {
    const std::array<std::string_view, 1> entry{"abuse"};
    for (unsigned i = 0; i < 10000; ++i)
      categories.match_and_increment(entry);
  };
  std::thread first(increment);
  std::thread second(increment);
  first.join();
  second.join();
  require(categories.blocked("abuse") == 20000,
          "concurrent increments were lost");
}

} // namespace

int main() {
  try {
    test_configuration_and_decisions();
    test_counter_updates();
    std::cout << "Drop-category tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
