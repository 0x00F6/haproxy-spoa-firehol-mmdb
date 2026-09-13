#include "firehol_blocklist_ipsets/compiler.hpp"
#include "firehol_blocklist_ipsets/git_repository.hpp"
#include "job_scheduler/job_scheduler.hpp"
#include <maxminddb.h>
#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace firehol_ipsets;
namespace fs = std::filesystem;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            throw std::runtime_error(msg); \
        } \
    } while (0)

class TestDirectory {
    fs::path original_path_ = fs::current_path();
    fs::path path_;

public:
    TestDirectory() {
        auto pattern = (fs::temp_directory_path() / "firehol_test_XXXXXX").string();
        char* directory = ::mkdtemp(pattern.data());
        if (!directory) {
            throw std::system_error(errno, std::generic_category(), "Create test directory");
        }
        path_ = directory;
        try {
            fs::current_path(path_);
        } catch (...) {
            std::error_code error;
            fs::remove_all(path_, error);
            throw;
        }
    }

    ~TestDirectory() {
        std::error_code error;
        fs::current_path(original_path_, error);
        fs::remove_all(path_, error);
    }

    const fs::path& path() const { return path_; }
};

std::string shell_quote(const fs::path& path) {
    std::string quoted = "'";
    for (char character : path.string()) {
        quoted += character == '\'' ? "'\\''" : std::string(1, character);
    }
    return quoted + "'";
}

void run_git(const fs::path& repo_path, const std::string& args) {
    const std::string command = "git -C " + shell_quote(repo_path) +
        " -c commit.gpgsign=false -c core.hooksPath=/dev/null " + args;
    CHECK(std::system(command.c_str()) == 0, "Git fixture command failed: " + args);
}

void setup_local_repo(const fs::path& repo_path) {
    fs::create_directories(repo_path);
    run_git(repo_path, "init -b master");
    run_git(repo_path, "config user.name Test");
    run_git(repo_path, "config user.email test@test.com");
    std::ofstream(repo_path / "test.netset") << "# test\n";
    run_git(repo_path, "add test.netset");
    run_git(repo_path, "commit -m 'Initial commit'");
}

void add_new_commit(const fs::path& repo_path) {
    std::ofstream(repo_path / "new.netset") << "10.0.0.0/8\n";
    run_git(repo_path, "add new.netset");
    run_git(repo_path, "commit -m 'Second commit'");
}

// Reads element `index` of the string array `key` from a columnar record.
static std::optional<std::string> column_value(MMDB_entry_s entry, const char* key, const char* index) {
    MMDB_entry_data_s data{};
    if (MMDB_get_value(&entry, &data, key, index, nullptr) != MMDB_SUCCESS || !data.has_data ||
        data.type != MMDB_DATA_TYPE_UTF8_STRING) {
        return std::nullopt;
    }
    return std::string(data.utf8_string, data.data_size);
}

void check_compiled_network(const fs::path& output_path) {
    MMDB_s mmdb{};
    CHECK(MMDB_open(output_path.c_str(), MMDB_MODE_MMAP, &mmdb) == MMDB_SUCCESS,
          "Scheduled MMDB should open successfully");
    std::unique_ptr<MMDB_s, decltype(&MMDB_close)> guard(&mmdb, &MMDB_close);
    int gai_error = 0;
    int mmdb_error = 0;
    auto result = MMDB_lookup_string(&mmdb, "10.1.2.3", &gai_error, &mmdb_error);
    CHECK(gai_error == 0 && mmdb_error == MMDB_SUCCESS && result.found_entry,
          "Scheduled MMDB should contain the fetched network");
    CHECK(column_value(result.entry, "file_name", "0") == "new.netset",
          "Scheduled MMDB should identify its source blocklist");
}

void check_native_thread_allocations() {
    // The compiler creates fresh parsing threads every run. Exercise more than
    // 256 thread lifetimes to catch exhaustion of Seastar's shard allocator IDs.
    for (unsigned iteration = 0; iteration < 300; ++iteration) {
        std::exception_ptr failure;
        std::jthread worker([&] {
            try {
                std::vector<unsigned char> buffer(1024, 0x5a);
                CHECK(std::accumulate(buffer.begin(), buffer.end(), size_t{0}) ==
                          buffer.size() * 0x5a,
                      "Repeated native threads must be able to allocate memory");
            } catch (...) {
                failure = std::current_exception();
            }
        });
        worker.join();
        if (failure) std::rethrow_exception(failure);
    }
}

void check_scheduled_compilation(const fs::path& remote, const fs::path& root) {
    const std::string repo_url = remote.string();
    const fs::path checkout = root / "scheduled checkout";
    const fs::path output = root / "scheduled.mmdb";
    const auto caller_thread = std::this_thread::get_id();
    const auto caller_pid = ::getpid();
    std::promise<void> completed;
    auto completion = completed.get_future();
    unsigned runs = 0;
    bool completion_sent = false;
    std::thread::id worker_thread;
    {
        job_scheduler::JobScheduler scheduler("* * * * * *", [&] {
            if (completion_sent) return;
            try {
                CHECK(::getpid() == caller_pid,
                      "Scheduled compilation must stay in the original process");
                CHECK(std::this_thread::get_id() != caller_thread,
                      "Scheduled compilation must run on a native worker thread");
                if (runs == 0) worker_thread = std::this_thread::get_id();
                CHECK(std::this_thread::get_id() == worker_thread,
                      "Repeated compilations should use the scheduler's existing thread");

                // Real blocklist files and builder buffers exceed Seastar's
                // small allocation reserve for threads outside the reactor.
                std::vector<unsigned char> buffer(64U * 1024U * 1024U, 0x5a);
                CHECK(std::accumulate(buffer.begin(), buffer.end(), size_t{0}) ==
                          buffer.size() * 0x5a,
                      "The scheduled worker must support large allocations");

                GitGlobalInit git_init;
                GitRepository repo;
                const auto repository = repo.prepare_repository(repo_url, checkout);
                compile_to_mmdb(output, repository, 0);
                check_compiled_network(output);
                if (++runs == 2) {
                    completion_sent = true;
                    completed.set_value();
                }
            } catch (...) {
                completion_sent = true;
                completed.set_exception(std::current_exception());
            }
        });
        CHECK(completion.wait_for(std::chrono::seconds(20)) == std::future_status::ready,
              "Two scheduled MMDB updates should finish");
        completion.get();
    }
    CHECK(runs == 2, "The same scheduler thread must successfully update the MMDB twice");
    check_compiled_network(output);
}

int run_tests() {
    TestDirectory directory;
    const fs::path test_remote = directory.path() / "remote";
    const fs::path test_local = directory.path() / "local";
    setup_local_repo(test_remote);

    GitGlobalInit git_init;

    std::cout << "Test 1: Clone" << std::endl;
    {
        GitRepository repo;
        repo.prepare_repository(test_remote.string(), test_local, "master");
        CHECK(fs::exists(test_local / "test.netset"), "File should exist after clone");
    }

    std::cout << "Test 2: Modify and Reset" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    {
        std::ofstream(test_local / "test.netset") << "modifications";
        std::ofstream(test_local / "untracked.txt") << "untracked";

        add_new_commit(test_remote);

        GitRepository repo;
        repo.prepare_repository(test_remote.string(), test_local, "master");

        std::ifstream file(test_local / "new.netset");
        std::string content;
        std::getline(file, content);
        CHECK(content == "10.0.0.0/8", "new.netset should have been fetched and checked out");

        std::ifstream mod_file(test_local / "test.netset");
        std::string mod_content;
        std::getline(mod_file, mod_content);
        CHECK(mod_content == "# test", "Hard reset should restore tracked content");
    }

    std::cout << "Test 3: Missing repository" << std::endl;
    {
        fs::remove_all(test_local);
        GitRepository repo;
        bool threw = false;
        try {
            repo.prepare_repository((directory.path() / "missing").string(), test_local, "master");
        } catch (const GitError&) {
            threw = true;
        }
        CHECK(threw, "Should throw for a missing repository");
    }

    std::cout << "Test 4: Compile MMDB" << std::endl;
    const fs::path output_path = directory.path() / "test_firehol.mmdb";
    compile_to_mmdb(output_path, test_remote, 0);

    MMDB_s mmdb{};
    CHECK(MMDB_open(output_path.c_str(), MMDB_MODE_MMAP, &mmdb) == MMDB_SUCCESS,
          "Compiled MMDB should open successfully");
    std::unique_ptr<MMDB_s, decltype(&MMDB_close)> mmdb_guard(&mmdb, &MMDB_close);

    int gai_error = 0;
    int mmdb_error = 0;
    auto result = MMDB_lookup_string(&mmdb, "10.1.2.3", &gai_error, &mmdb_error);
    CHECK(gai_error == 0 && mmdb_error == MMDB_SUCCESS && result.found_entry,
          "Compiled MMDB should contain the fetched network");
    CHECK(column_value(result.entry, "file_name", "0") == "new.netset",
          "Compiled network should identify its source list");
    CHECK(column_value(result.entry, "category", "0") == "",
          "Blocklists without metadata keep an empty category column");

    std::cout << "Test 5: Invalid blocklist preserves the existing MMDB" << std::endl;
    {
        std::ifstream original_file(output_path, std::ios::binary);
        const std::string original_content{std::istreambuf_iterator<char>(original_file), {}};
        std::ofstream(test_remote / "invalid.netset") << "not-a-network\n";

        bool threw = false;
        try {
            compile_to_mmdb(output_path, test_remote, 0);
        } catch (const std::runtime_error& error) {
            threw = true;
            CHECK(std::string_view(error.what()).find("invalid.netset:1") !=
                      std::string_view::npos,
                  "Parsing errors should identify the invalid source line");
        }
        CHECK(threw, "Invalid blocklist should propagate a compilation error");
        std::ifstream preserved_file(output_path, std::ios::binary);
        const std::string preserved_content{std::istreambuf_iterator<char>(preserved_file), {}};
        CHECK(preserved_content == original_content,
              "A compilation error must preserve the existing MMDB");
    }

    std::cout << "Test 6: Repeated native thread allocations" << std::endl;
    check_native_thread_allocations();

    std::cout << "Test 7: Scheduled Git preparation and compilation in the same process"
              << std::endl;
    check_scheduled_compilation(test_remote, directory.path());

    std::cout << "All Git repository and FireHOL compiler tests passed!" << std::endl;
    return 0;
}

int main(int argc, char** argv) {
    try {
        // Run with the application's Seastar runtime to exercise native worker
        // allocations under the same linked allocator configuration.
        seastar::app_template::seastar_options options;
        options.smp_opts.smp.set_value(1);
        options.smp_opts.memory.set_value("512M");
        options.reactor_opts.network_stack.select_candidate("posix");
        options.reactor_opts.overprovisioned.set_value();
        seastar::app_template app(std::move(options));
        app.set_configuration_reader([](auto&) {});
        return app.run(argc, argv, [] {
            return seastar::async([] {
                return run_tests();
            });
        });
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
