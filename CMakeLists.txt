cmake_minimum_required(VERSION 3.24)

set(CMAKE_C_COMPILER gcc-15)
set(CMAKE_CXX_COMPILER g++-15)
project(haproxy-spoa-firehol-mmdb LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-fcoroutines)
# Project sources (not the vendored dependencies) build warning-free.
add_compile_options(-Wall -Wextra)

# ---------------------------------------------------------------------------
# Project version for --version. Use the last git tag (e.g. "1.9.0") if one
# exists, otherwise fall back to the short hash of the last commit.
# ---------------------------------------------------------------------------
find_package(Git QUIET)
set(SPOA_SERVER_VERSION "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE _git_version
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_git_version)
        set(SPOA_SERVER_VERSION "${_git_version}")
    endif()
endif()
message(STATUS "SPOA_SERVER_VERSION: ${SPOA_SERVER_VERSION}")

# Build statically by default. NSS lookups and dlopen used by dependencies
# still carry glibc runtime requirements in this mode.
option(BUILD_FULL_STATIC "Link everything statically (glibc runtime caveats apply)"
       ON)

if(BUILD_FULL_STATIC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
endif()

# ---------------------------------------------------------------------------
# Release optimizations mirroring Rust's [profile.release]:
#   opt-level     = 3      -> -O3
#   lto           = "fat"  -> fat (whole-program) LTO
#   codegen-units = 1      -> LTO treats the app as a single unit (see below)
#   panic         = "abort"-> not mapped: would require -fno-exceptions which
#                            breaks Seastar/GnuTLS/libstdc++ exception use
#   strip         = false  -> keep symbols (don't pass -s)
#
# The C++ equivalent of Rust's codegen-units is controlling how the optimizer
# splits the program during LTO. GCC's fat LTO already plans a single unit by
# default; -flto-partition=none forces that single whole-partition build.
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the build type" FORCE)
endif()

# Guarantee -O3 is configured for Release and RelWithDebInfo builds
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "Flags used by C compiler during Release builds" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "Flags used by CXX compiler during Release builds" FORCE)
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O3 -g -DNDEBUG" CACHE STRING "Flags used by C compiler during RelWithDebInfo builds" FORCE)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -g -DNDEBUG" CACHE STRING "Flags used by CXX compiler during RelWithDebInfo builds" FORCE)

option(SPOE_PROFILE "Keep symbols and debug information for CPU profiling" OFF)
if(SPOE_PROFILE)
    add_compile_options(-g1)
    add_link_options(-g1)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR NOT CMAKE_BUILD_TYPE)
    add_compile_options(-O3)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        set(_release_cxx_flags -flto -flto-partition=none)
        set(_release_link_flags -flto -flto-partition=none)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_release_cxx_flags -flto=full)
        set(_release_link_flags -flto=full)
    endif()
    add_compile_options(${_release_cxx_flags})
    add_link_options(${_release_link_flags})
    # strip = true -> strip symbol/debug info from the final binary
    if(NOT SPOE_PROFILE AND (CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_BUILD_TYPE))
        add_link_options(-s)
    endif()
endif()

include(cmake/Dependencies.cmake)
include(cmake/Argparse.cmake)

find_package(Threads REQUIRED)

add_library(spoa_core STATIC
    src/app_config.cpp
    src/spoa/frame.cpp
    src/spoa/typed_data.cpp
    src/mmdb/mmdb.cpp
    src/spoa/drop_categories.cpp
    src/job_scheduler/job_scheduler.cpp
    src/mmdb_builder/builder.cpp)
target_include_directories(spoa_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Keep the synchronous core independent of the Seastar runtime.
target_link_libraries(spoa_core PUBLIC libmaxminddb::libmaxminddb Threads::Threads atomic)

add_executable(haproxy-spoa-firehol-mmdb
    src/main.cpp
    src/mmdb/mmdb_reload.cpp
    src/firehol_blocklist_ipsets/compiler.cpp
    src/firehol_blocklist_ipsets/git_repository.cpp
    src/metrics/firehol_metrics.cpp
  src/metrics/scheduler_metrics.cpp
  src/metrics/spoa_metrics.cpp
    src/spoa/spoa_server.cpp
    src/spoa/spoa_agent.cpp)

# All third-party link inputs are explicit local .a archives. Only the
# compiler/libc runtime (Threads, dl, rt, m, atomic) comes from the toolchain.
# Rescan the static archives to resolve circular dependencies (GnuTLS/Nettle).
target_link_libraries(haproxy-spoa-firehol-mmdb PRIVATE
    argparse::argparse
    -Wl,--start-group
    spoa_core seastar::seastar ${SPOE_SOURCE_LIBRARIES}
    -Wl,--end-group
    Threads::Threads ${CMAKE_DL_LIBS} rt m atomic)

target_compile_definitions(haproxy-spoa-firehol-mmdb PRIVATE
    SPOA_SERVER_VERSION="${SPOA_SERVER_VERSION}")

add_executable(benchmark_spoa
    tests/benchmark_spoa.cpp)
target_include_directories(benchmark_spoa PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(benchmark_spoa PRIVATE
    argparse::argparse
    spoa_core
    Threads::Threads)


if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_executable(spoe_parse_test tests/test_parser.cpp)
    target_link_libraries(spoe_parse_test PRIVATE spoa_core)
    add_test(NAME spoe_parse_test COMMAND spoe_parse_test)

    add_executable(drop_categories_test tests/test_drop_categories.cpp)
    target_link_libraries(drop_categories_test PRIVATE spoa_core)
    add_test(NAME drop_categories_test COMMAND drop_categories_test)

    add_executable(mmdb_test tests/test_mmdb.cpp)
    target_link_libraries(mmdb_test PRIVATE spoa_core)
    add_test(NAME mmdb_test COMMAND mmdb_test)

    add_executable(mmdb_reload_test tests/test_mmdb_reload.cpp)
    target_link_libraries(mmdb_reload_test PRIVATE -Wl,--start-group spoa_core seastar::seastar ${SPOE_SOURCE_LIBRARIES} -Wl,--end-group Threads::Threads rt m atomic)
target_sources(mmdb_reload_test PRIVATE src/mmdb/mmdb_reload.cpp)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/firehol.mmdb")
        add_test(NAME mmdb_reload_test
            COMMAND mmdb_reload_test "${CMAKE_CURRENT_SOURCE_DIR}/firehol.mmdb")
    endif()

    add_executable(spoa_agent_test tests/test_agent.cpp)
    target_compile_options(spoa_agent_test PRIVATE -UNDEBUG)
    target_include_directories(spoa_agent_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(spoa_agent_test PRIVATE spoa_core Threads::Threads)
    set(_agent_test_args --binary $<TARGET_FILE:haproxy-spoa-firehol-mmdb>)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/firehol.mmdb")
        list(APPEND _agent_test_args --mmdb "${CMAKE_CURRENT_SOURCE_DIR}/firehol.mmdb")
    endif()
    add_test(NAME spoa_agent_test COMMAND spoa_agent_test ${_agent_test_args})
    set_tests_properties(spoa_agent_test PROPERTIES TIMEOUT 60)

    add_executable(flamegraph_test tests/test_flamegraph.cpp)
    target_compile_options(flamegraph_test PRIVATE -UNDEBUG)
    target_include_directories(flamegraph_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(flamegraph_test PRIVATE Threads::Threads)
    add_test(NAME flamegraph_test COMMAND flamegraph_test)
    set_tests_properties(flamegraph_test PROPERTIES TIMEOUT 60)

    add_executable(mmdb_builder_test tests/test_builder.cpp)
    target_include_directories(mmdb_builder_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(mmdb_builder_test PRIVATE spoa_core Threads::Threads)
    add_test(NAME mmdb_builder_test COMMAND mmdb_builder_test)
    set_tests_properties(mmdb_builder_test PROPERTIES TIMEOUT 60)

    add_executable(mmdb_builder_determinism_test tests/test_builder_determinism.cpp)
    target_link_libraries(mmdb_builder_determinism_test PRIVATE spoa_core Threads::Threads)
    add_test(NAME mmdb_builder_determinism_test COMMAND mmdb_builder_determinism_test)
    set_tests_properties(mmdb_builder_determinism_test PROPERTIES TIMEOUT 60)

    add_executable(utils_test tests/test_utils.cpp)
    target_link_libraries(utils_test PRIVATE spoa_core)
    add_test(NAME utils_test COMMAND utils_test)

    # Throughput benchmark for the MMDB builder (not a test; run manually).
    add_executable(benchmark_builder tests/benchmark_builder.cpp)
    target_link_libraries(benchmark_builder PRIVATE spoa_core Threads::Threads)


    # Logger instances are registered before main().  Run the lightweight
    # --help path to catch startup exceptions (such as a duplicate Seastar
    # logger) without needing sockets or elevated networking capabilities.
    add_test(
        NAME spoa_startup_test
        COMMAND ${CMAKE_COMMAND}
            -DPROGRAM=$<TARGET_FILE:haproxy-spoa-firehol-mmdb>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/spoa_startup_test.cmake)
endif()

    add_executable(firehol_compiler_test tests/test_firehol_compiler.cpp src/firehol_blocklist_ipsets/compiler.cpp src/firehol_blocklist_ipsets/git_repository.cpp src/metrics/firehol_metrics.cpp
  src/metrics/scheduler_metrics.cpp
  src/metrics/spoa_metrics.cpp)
    target_include_directories(firehol_compiler_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(firehol_compiler_test PRIVATE -Wl,--start-group spoa_core seastar::seastar ${SPOE_SOURCE_LIBRARIES} -Wl,--end-group Threads::Threads rt m atomic)
    add_test(NAME firehol_compiler_test COMMAND firehol_compiler_test)
    set_tests_properties(firehol_compiler_test PROPERTIES TIMEOUT 120)

    add_executable(firehol_job_test tests/test_firehol_job.cpp)
    target_link_libraries(firehol_job_test PRIVATE spoa_core)
    add_dependencies(firehol_job_test haproxy-spoa-firehol-mmdb)
    add_test(NAME firehol_job_test
        COMMAND firehol_job_test --binary $<TARGET_FILE:haproxy-spoa-firehol-mmdb>)
    set_tests_properties(firehol_job_test PROPERTIES TIMEOUT 180)

    add_executable(job_scheduler_test tests/test_job_scheduler.cpp)
    target_include_directories(job_scheduler_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(job_scheduler_test PRIVATE spoa_core Threads::Threads atomic)
    add_test(NAME job_scheduler_test COMMAND job_scheduler_test)
    set_tests_properties(job_scheduler_test PROPERTIES TIMEOUT 120)
