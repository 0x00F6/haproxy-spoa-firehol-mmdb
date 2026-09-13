# seastar 25.05.0: built from source as static archives with -O3.
# Use the system allocator for the native scheduler thread as well as reactors.
# Seastar 25.05 has no CMake option for this supported allocator mode.
include_guard(GLOBAL)
spoe_cmake(seastar
    VERSION 25.05.0
    URL "https://github.com/scylladb/seastar/archive/refs/tags/seastar-25.05.0.tar.gz"
    SHA256 6e0405706a539af5a0ee307278bbd1fd965a2d97f7c8b970b7daa64d4ddfae11
    ARCHIVES seastar
    DEPENDS boost cares fmt lz4 gnutls uring hwloc sctp yaml protobuf
        openssl snappy zstd aio maxminddb xfs dpdk
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/PatchSeastar.cmake
    CMAKE_ARGS -DSeastar_APPS=OFF -DSeastar_DEMOS=OFF -DSeastar_DOCS=OFF
        -DSeastar_TESTING=OFF -DSeastar_DPDK=ON -DSeastar_IO_URING=ON
        -DSeastar_INSTALL=ON -DSeastar_CXX_FLAGS=-DNO_EXCEPTION_INTERCEPT
        "-DCMAKE_CXX_FLAGS=-O3 -DSEASTAR_DEFAULT_ALLOCATOR"
        -DCMAKE_CXX_STANDARD=20 -DBoost_USE_STATIC_LIBS=ON -DBoost_USE_STATIC_RUNTIME=ON
        -DBOOST_ROOT=${SPOE_SOURCE_PREFIX} -DBoost_NO_SYSTEM_PATHS=ON
        -Dfmt_DIR=${SPOE_SOURCE_PREFIX}/lib/cmake/fmt
        -DGnuTLS_LIBRARY=${SPOE_SOURCE_PREFIX}/lib/libgnutls.a
        -DGnuTLS_INCLUDE_DIR=${SPOE_SOURCE_PREFIX}/include
        -Dhwloc_LIBRARY=${SPOE_SOURCE_PREFIX}/lib/libhwloc.a
        -Dhwloc_INCLUDE_DIR=${SPOE_SOURCE_PREFIX}/include
        -Dlz4_LIBRARY=${SPOE_SOURCE_PREFIX}/lib/liblz4.a
        -Dlz4_INCLUDE_DIR=${SPOE_SOURCE_PREFIX}/include
        -DURING_LIBRARY=${SPOE_SOURCE_PREFIX}/lib/liburing.a
        -DURING_INCLUDE_DIR=${SPOE_SOURCE_PREFIX}/include
        -Dc-ares_LIBRARY=${SPOE_SOURCE_PREFIX}/lib/libcares.a
        -Dc-ares_INCLUDE_DIR=${SPOE_SOURCE_PREFIX}/include
        -Dlksctp-tools_LIBRARY=${SPOE_SOURCE_PREFIX}/lib/libsctp.a
        -Dlksctp-tools_INCLUDE_DIR=${SPOE_SOURCE_PREFIX}/include
        -Dyaml-cpp_LIBRARY_RELEASE=${SPOE_SOURCE_PREFIX}/lib/libyaml-cpp.a
        -Dyaml-cpp_INCLUDE_DIR=${SPOE_SOURCE_PREFIX}/include
        -DProtobuf_PROTOC_EXECUTABLE=${SPOE_SOURCE_PREFIX}/bin/protoc
        -DProtobuf_DIR=${SPOE_SOURCE_PREFIX}/lib/cmake/protobuf
)
