cmake_minimum_required(VERSION 3.24)
project(spoa_source_dependencies LANGUAGES C CXX)
if(NOT BUILD_FULL_STATIC)
    message(FATAL_ERROR "The dependency build produces static archives; BUILD_FULL_STATIC must be ON")
endif()
include(../SourceLibrary.cmake)
foreach(module GMP Nettle LibTASN1 LibUnistring Zlib Numa SCTP LibMaxMindDB
        GnuTLS Hwloc Fmt CAres YamlCpp Snappy LZ4 Zstd Protobuf OpenSSL
        Boost LibUring LibAIO LibUUID XfsHeaders DPDK Seastar LibGit2)
    include(../${module}.cmake)
endforeach()

get_property(_cache_libraries GLOBAL PROPERTY SPOE_CACHE_LIBRARIES)
string(REPLACE ";" "\n" _cache_libraries "${_cache_libraries}")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/dependency-cache-list.txt"
    CONTENT "${_cache_libraries}\n")
