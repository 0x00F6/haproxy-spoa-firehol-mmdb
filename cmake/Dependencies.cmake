# Bootstrap third-party libraries before configuring the application so their
# generated headers and CMake package metadata are available on the first build.
include_guard(GLOBAL)
set(SPOE_DEPS_CACHE_DIR "${CMAKE_SOURCE_DIR}/.cache/deps" CACHE PATH
    "Persistent cache for external dependency builds")
set(SPOE_DEPS_JOBS 2 CACHE STRING "Parallel jobs per dependency build")
if(NOT SPOE_DEPS_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "SPOE_DEPS_JOBS must be a positive integer")
endif()
# Independent modules build concurrently; DEPENDS in each recipe orders the
# rest. The default keeps SPOA_DEPS_PARALLEL * SPOE_DEPS_JOBS compile jobs
# within the CPU count and roughly 1.5 GiB of RAM per job.
set(SPOA_DEPS_PARALLEL "" CACHE STRING
    "Dependency modules built concurrently (empty: derived from CPUs and RAM)")
if(SPOA_DEPS_PARALLEL STREQUAL "")
    cmake_host_system_information(RESULT _cpus QUERY NUMBER_OF_LOGICAL_CORES)
    cmake_host_system_information(RESULT _ram_mib QUERY TOTAL_PHYSICAL_MEMORY)
    math(EXPR _jobs_by_ram "${_ram_mib} / 1536")
    if(_jobs_by_ram LESS _cpus)
        set(_cpus "${_jobs_by_ram}")
    endif()
    math(EXPR _deps_parallel "${_cpus} / ${SPOE_DEPS_JOBS}")
    if(_deps_parallel LESS 1)
        set(_deps_parallel 1)
    endif()
    unset(_cpus)
    unset(_ram_mib)
    unset(_jobs_by_ram)
elseif(SPOA_DEPS_PARALLEL MATCHES "^[1-9][0-9]*$")
    set(_deps_parallel "${SPOA_DEPS_PARALLEL}")
else()
    message(FATAL_ERROR "SPOA_DEPS_PARALLEL must be empty or a positive integer")
endif()
# Do not reuse archives from the previous system-dependency build. Compiler
# changes get a separate prefix; ExternalProject stamps track recipe changes.
set(_toolchain "${CMAKE_C_COMPILER};${CMAKE_C_COMPILER_VERSION};${CMAKE_CXX_COMPILER};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_SYSTEM_PROCESSOR}")
string(SHA256 _toolchain_id "${_toolchain}")
string(SUBSTRING "${_toolchain_id}" 0 12 _toolchain_id)
set(SPOE_SOURCE_BUILD "${SPOE_DEPS_CACHE_DIR}/source-static-v1/${_toolchain_id}")
set(SPOE_SOURCE_PREFIX "${SPOE_SOURCE_BUILD}/prefix")
# An IDE reload and make build can otherwise configure the same Autotools
# tree concurrently, regenerating headers while another process compiles it.
# Say so once when the lock is busy; otherwise the configure looks frozen.
# Then retry silently in 10-second slices, for at most one hour.
file(MAKE_DIRECTORY "${SPOE_SOURCE_BUILD}")
file(LOCK "${SPOE_SOURCE_BUILD}.lock" GUARD FILE TIMEOUT 0 RESULT_VARIABLE _lock_status)
if(NOT _lock_status STREQUAL "0")
    message(STATUS "⏳ Another configure (IDE reload?) holds ${SPOE_SOURCE_BUILD}.lock; waiting")
    foreach(_attempt RANGE 359)
        file(LOCK "${SPOE_SOURCE_BUILD}.lock" GUARD FILE TIMEOUT 10 RESULT_VARIABLE _lock_status)
        if(_lock_status STREQUAL "0")
            break()
        endif()
    endforeach()
    if(NOT _lock_status STREQUAL "0")
        message(FATAL_ERROR "Dependency lock still held after one hour: ${SPOE_SOURCE_BUILD}.lock")
    endif()
endif()
unset(_lock_status)
unset(_attempt)
file(GLOB _dependency_recipes CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/*.cmake")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_dependency_recipes} "${CMAKE_CURRENT_LIST_DIR}/dependencies/CMakeLists.txt")
message(STATUS "📦 Static source dependencies (-O3): ${SPOE_SOURCE_PREFIX}")
# IDE and terminal builds may run different CMake binaries. ExternalProject
# embeds the configuring binary in every step script, so alternating binaries
# rewrite the scripts and re-run every dependency step. Drive the dependency
# tree with the first cmake on PATH, identical from both sides, and fall back
# to the running binary when there is none.
find_program(SPOE_DEPS_CMAKE NAMES cmake
    DOC "CMake binary that configures and builds the dependency tree")
if(NOT SPOE_DEPS_CMAKE)
    set(SPOE_DEPS_CMAKE "${CMAKE_COMMAND}")
endif()
execute_process(COMMAND ${SPOE_DEPS_CMAKE}
    -S "${CMAKE_CURRENT_LIST_DIR}/dependencies" -B "${SPOE_SOURCE_BUILD}" -G Ninja
    -DBUILD_FULL_STATIC=ON -DSPOE_DEPS_CACHE_DIR=${SPOE_DEPS_CACHE_DIR}
    -DSPOE_SOURCE_PREFIX=${SPOE_SOURCE_PREFIX} -DSPOE_DEPS_JOBS=${SPOE_DEPS_JOBS}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    COMMAND_ERROR_IS_FATAL ANY)
# Ask Ninja whether each target (including its prerequisites) is up to date.
# Archive existence alone would incorrectly report hits after recipe changes.
file(STRINGS "${SPOE_SOURCE_BUILD}/dependency-cache-list.txt" _cache_libraries)
foreach(_library IN LISTS _cache_libraries)
    string(REGEX REPLACE " .*" "" _target "${_library}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E env --unset=MAKEFLAGS
        ${SPOE_DEPS_CMAKE} --build "${SPOE_SOURCE_BUILD}" --target "${_target}" -- -n
        OUTPUT_VARIABLE _cache_status
        COMMAND_ERROR_IS_FATAL ANY)
    if(_cache_status MATCHES "ninja: no work to do\\.")
        message(STATUS "⚡ Cache hit: ${_library}; no rebuild needed.")
    endif()
endforeach()
unset(_cache_libraries)
unset(_library)
unset(_target)
unset(_cache_status)

# Ninja rewrites one status line on a terminal, hiding a long build; TERM=dumb
# logs each step on its own line and NINJA_STATUS adds the elapsed time.
message(STATUS "🔨 Building source dependencies: ${_deps_parallel} module(s) at a time, "
    "${SPOE_DEPS_JOBS} job(s) each (step logs: ${SPOE_SOURCE_BUILD}/<name>/stamp)")
execute_process(COMMAND ${CMAKE_COMMAND} -E env --unset=MAKEFLAGS
    TERM=dumb "NINJA_STATUS=[%f/%t %es] "
    ${SPOE_DEPS_CMAKE} --build "${SPOE_SOURCE_BUILD}" --parallel ${_deps_parallel}
    COMMAND_ERROR_IS_FATAL ANY)
message(STATUS "✅ Source dependencies ready")
unset(_deps_parallel)

function(spoe_import_library name archive)
    set(_path "${SPOE_SOURCE_PREFIX}/lib/lib${archive}.a")
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR "Missing source-built archive: ${_path}")
    endif()
    add_library(${name} STATIC IMPORTED GLOBAL)
    set_target_properties(${name} PROPERTIES IMPORTED_LOCATION "${_path}"
        INTERFACE_INCLUDE_DIRECTORIES "${SPOE_SOURCE_PREFIX}/include")
endfunction()
spoe_import_library(seastar::seastar seastar)
spoe_import_library(libmaxminddb::libmaxminddb maxminddb)
set_target_properties(seastar::seastar PROPERTIES
    INTERFACE_COMPILE_FEATURES cxx_std_20
    INTERFACE_COMPILE_OPTIONS "-fcoroutines;-fexceptions;-fno-strict-aliasing"
    INTERFACE_COMPILE_DEFINITIONS "SEASTAR_API_LEVEL=7;SEASTAR_SCHEDULING_GROUPS_COUNT=16;SEASTAR_SSTRING;SEASTAR_DEPRECATED_OSTREAM_FORMATTERS;SEASTAR_LOGGER_COMPILE_TIME_FMT;SEASTAR_HAVE_DPDK;SEASTAR_DEFAULT_ALLOCATOR")
# Seastar embeds the static DPDK objects in libseastar.a. Mirror its public
# DPDK header/CPU requirements on our imported target as well.
set_property(TARGET seastar::seastar APPEND PROPERTY
    INTERFACE_INCLUDE_DIRECTORIES "${SPOE_SOURCE_PREFIX}/include/dpdk")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64")
    set_property(TARGET seastar::seastar APPEND PROPERTY
        INTERFACE_COMPILE_OPTIONS -mcpu=native -mtune=native)
else()
    set_property(TARGET seastar::seastar APPEND PROPERTY
        INTERFACE_COMPILE_OPTIONS -march=native)
endif()
set(SPOE_SOURCE_LIBRARIES)
foreach(lib boost_program_options boost_thread boost_atomic boost_chrono
        boost_date_time boost_filesystem cares sctp crypto ssl protobuf gnutls
        hogweed nettle gmp tasn1 unistring hwloc yaml-cpp fmt lz4 snappy zstd
        uring aio numa z uuid git2)
    spoe_import_library(spoe::${lib} ${lib})
    list(APPEND SPOE_SOURCE_LIBRARIES spoe::${lib})
endforeach()
