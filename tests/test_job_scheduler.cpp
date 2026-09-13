#include "job_scheduler/job_scheduler.hpp"
#include "job_scheduler/croncpp.h"
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <cassert>
#include <future>

using namespace job_scheduler;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL: " << msg << std::endl; \
            std::abort(); \
        } \
    } while(0)

void test_invalid_cron() {
    std::cout << "Running test_invalid_cron..." << std::endl;
    bool caught = false;
    try {
        JobScheduler scheduler("invalid cron expr", []{});
    } catch (const std::invalid_argument&) {
        caught = true;
    }
    CHECK(caught, "Should throw invalid_argument on bad cron");
}

void test_valid_cron() {
    std::cout << "Running test_valid_cron..." << std::endl;
    std::atomic<int> counter{0};
    {
        // Executes exactly once immediately if time aligns, else might take 1s.
        // We use wait pattern. Using '* * * * * *' executes every second.
        JobScheduler scheduler("* * * * * *", [&counter] {
            counter++;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    }
    
    int c = counter.load();
    CHECK(c >= 2, "Job should run multiple times");
    std::cout << "Job executed " << c << " times." << std::endl;
}

void test_hourly_cron() {
    std::cout << "Running test_hourly_cron..." << std::endl;
    // The scheduler accepts UNIX crontab expressions, adding zero seconds.
    JobScheduler scheduler("0 * * * *", [] {});
    const auto hourly = cron::make_cron("0 0 * * * *");

    for (const auto* start : {"2026-01-15 10:00:00", "2026-01-15 10:37:42"}) {
        const auto next = cron::cron_next(hourly, cron::utils::to_tm(start));
        CHECK(next.tm_year == 126 && next.tm_mon == 0 && next.tm_mday == 15,
              "The next hourly occurrence should stay on the same day");
        CHECK(next.tm_hour == 11 && next.tm_min == 0 && next.tm_sec == 0,
              "Hourly cron should run at the beginning of the next hour");
    }

    const auto midnight = cron::cron_next(hourly, cron::utils::to_tm("2026-01-15 23:59:59"));
    CHECK(midnight.tm_year == 126 && midnight.tm_mon == 0 && midnight.tm_mday == 16,
          "Hourly cron should advance to the next day at midnight");
    CHECK(midnight.tm_hour == 0 && midnight.tm_min == 0 && midnight.tm_sec == 0,
          "Hourly cron should run exactly at midnight");
}

void test_stop_during_wait() {
    std::cout << "Running test_stop_during_wait..." << std::endl;
    std::atomic<bool> ran{false};
    auto scheduler = std::make_unique<JobScheduler>("0 0 1 1 *", [&ran] {
        // Scheduled far in the future
        ran = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto stop_started = std::chrono::steady_clock::now();
    scheduler.reset();
    CHECK(std::chrono::steady_clock::now() - stop_started < std::chrono::seconds(1),
          "Stopping should interrupt the wait for a distant job");
    CHECK(!ran.load(), "Job should not have run");
}

void test_stop_during_job() {
    std::cout << "Running test_stop_during_job..." << std::endl;
    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> release;
    auto release_future = release.get_future();
    std::promise<void> finished;
    auto finished_future = finished.get_future();

    auto scheduler = std::make_unique<JobScheduler>("* * * * * *", [&] {
        started.set_value();
        release_future.wait();
        finished.set_value();
    });
    CHECK(started_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
          "The job should start before shutdown is requested");

    std::promise<void> stopping;
    auto stopping_future = stopping.get_future();
    std::promise<void> stopped;
    auto stopped_future = stopped.get_future();
    std::jthread shutdown([scheduler = std::move(scheduler), &stopping, &stopped]() mutable {
        stopping.set_value();
        scheduler.reset();
        stopped.set_value();
    });
    CHECK(stopping_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
          "The shutdown thread should start");
    CHECK(stopped_future.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout,
          "Shutdown should wait for the running job to finish");

    release.set_value();
    CHECK(stopped_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
          "Shutdown should finish after the job completes");
    CHECK(finished_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready,
          "The running job should complete before scheduler destruction returns");
}

void test_exception_in_job() {
    std::cout << "Running test_exception_in_job..." << std::endl;
    std::atomic<int> counter{0};
    {
        JobScheduler scheduler("* * * * * *", [&counter] {
            counter++;
            throw std::runtime_error("Test exception");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    }
    int c = counter.load();
    CHECK(c >= 2, "Scheduler should survive exception and continue running");
    std::cout << "Job executed " << c << " times with exceptions." << std::endl;
}

void test_concurrency_prevention() {
    std::cout << "Running test_concurrency_prevention..." << std::endl;
    std::atomic<int> concurrent_executions{0};
    std::atomic<bool> failed_concurrency{false};
    std::atomic<int> runs{0};

    {
        // Planifié 1 fois par seconde '*'
        JobScheduler scheduler("* * * * * *", [&] {
            int current = ++concurrent_executions;
            if (current > 1) {
                failed_concurrency = true;
            }
            
            // Simule un job long qui déborde sur la seconde d'après
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            
            concurrent_executions--;
            runs++;
        });
        
        std::this_thread::sleep_for(std::chrono::milliseconds(4000));
    }

    CHECK(!failed_concurrency.load(), "Job ran concurrently but should not have");
    CHECK(runs.load() > 0, "Job should have run at least once");
    std::cout << "Concurrency prevented. Job executed " << runs.load() << " times sequentially." << std::endl;
}

int main() {
    test_invalid_cron();
    test_hourly_cron();
    test_stop_during_wait();
    test_stop_during_job();
    test_valid_cron();
    test_exception_in_job();
    test_concurrency_prevention();
    
    std::cout << "All job scheduler tests passed!" << std::endl;
    return 0;
}
