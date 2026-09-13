#include "mmdb/mmdb_reload.h"

#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

void test_database_wrapper(const std::string &path) {
  spoe::Mmdb database(path);
  const auto metadata = database.metadata_string();
  database.open(database.path());
  require(database.path() == path && database.metadata_string() == metadata,
          "reopening with the database's own path failed");

  spoe::Mmdb moved(std::move(database));
  require(!database.is_open() && moved.is_open(),
          "move did not transfer ownership");
  database = std::move(moved);
  require(database.is_open() && !moved.is_open(),
          "move assignment lost ownership");

  const auto compare = [](const auto &binary, const auto &text) {
    require(binary.found_entry == text.found_entry &&
                binary.netmask == text.netmask,
            "binary and text lookups disagree");
    if (binary.found_entry)
      require(binary.entry.offset == text.entry.offset,
              "lookup entries disagree");
  };
  int binary_error = 0;
  int text_error = 0;
  auto ipv4 = database.lookup_ipv4({127, 0, 0, 1}, &binary_error);
  // A frame's address string can end before its backing buffer does.
  auto text4 = database.lookup_string(std::string_view("127.0.0.1trailing", 9),
                                      &text_error);
  require(binary_error == MMDB_SUCCESS && text_error == MMDB_SUCCESS,
          "IPv4 lookup failed");
  compare(ipv4, text4);

  std::array<uint8_t, 16> loopback6{};
  loopback6.back() = 1;
  auto ipv6 = database.lookup_ipv6(loopback6, &binary_error);
  auto text6 = database.lookup_string("::1", &text_error);
  require(binary_error == text_error, "IPv6 lookup errors disagree");
  compare(ipv6, text6);

  database.lookup_string("not-an-ip", &text_error);
  require(text_error != MMDB_SUCCESS, "invalid address was accepted");
  bool failed = false;
  try {
    database.open(path + ".missing");
  } catch (const std::runtime_error &) {
    failed = true;
  }
  require(failed && !database.is_open() && database.path().empty(),
          "failed reopen retained the previous database");
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  char directory[] = "/tmp/spoa-mmdb-test-XXXXXX";
  if (!mkdtemp(directory))
    return 2;
  struct Cleanup {
    const char *path;
    ~Cleanup() { fs::remove_all(path); }
  } cleanup{directory};
  try {
    const auto path = fs::path(directory) / "firehol.mmdb";
    fs::copy_file(argv[1], path);
    test_database_wrapper(path.string());
    spoe::ReloadableMmdb db;
    db.open(path.string());
    auto original = db.snapshot();
    const auto expected = original->metadata_string();
    auto original_entry = original->lookup_string("127.0.0.1");
    const auto retained_categories = spoe::Mmdb::get_categories(original_entry);
    std::vector<std::string> expected_categories;
    for (const auto category : retained_categories)
      expected_categories.emplace_back(category);
    const auto wait_for_reload = [&](const auto &previous) {
      const auto deadline = std::chrono::steady_clock::now() + 10s;
      while (db.snapshot() == previous &&
             std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
      require(db.snapshot() != previous, "reload timed out");
      require(db.snapshot()->metadata_string() == expected,
              "metadata mismatch");
    };
    // Invalid in-place writes must not damage the live mapping or replace it.
    {
      std::ofstream invalid(path);
      invalid << "invalid database";
    }
    std::this_thread::sleep_for(500ms);
    require(db.snapshot() == original, "invalid database was published");
    require(db.reload_count() == 0, "failed reload was counted as successful");
    int error = 0;
    original->lookup_string("127.0.0.1", &error);
    require(error == MMDB_SUCCESS, "old mapping corrupted by truncation");
    fs::copy_file(argv[1], path, fs::copy_options::overwrite_existing);
    wait_for_reload(original);
    // A rewrite may emit both IN_ATTRIB and IN_CLOSE_WRITE, with a second
    // reload still pending. Use a fresh watcher to isolate unrelated events.
    spoe::ReloadableMmdb unrelated_watcher;
    unrelated_watcher.open(path.string());
    auto unrelated_original = unrelated_watcher.snapshot();
    fs::copy_file(argv[1], fs::path(directory) / "unrelated.mmdb");
    std::this_thread::sleep_for(300ms);
    require(unrelated_watcher.snapshot() == unrelated_original,
            "unrelated file triggered reload");
    auto rewritten = db.snapshot();
    fs::rename(fs::path(directory) / "unrelated.mmdb", path);
    wait_for_reload(rewritten);
    auto renamed = db.snapshot();
    const auto count_before_touch = db.reload_count();
    require(utimensat(AT_FDCWD, path.c_str(), nullptr, 0) == 0, "touch failed");
    wait_for_reload(renamed);
    const auto count_deadline = std::chrono::steady_clock::now() + 1s;
    while (db.reload_count() == count_before_touch &&
           std::chrono::steady_clock::now() < count_deadline)
      std::this_thread::sleep_for(1ms);
    require(db.reload_count() > count_before_touch,
            "touch reload was not counted");
    require(original->metadata_string() == expected,
            "retained snapshot invalidated");
    for (size_t i = 0; i < retained_categories.size(); ++i)
      require(retained_categories[i] == expected_categories[i],
              "reload invalidated retained category views");
    std::cout << "MMDB reload tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
