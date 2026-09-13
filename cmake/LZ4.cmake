# lz4 1.10.0: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_cmake(lz4
    VERSION 1.10.0
    URL "https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz"
    SHA256 537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
    ARCHIVES lz4
    SOURCE_SUBDIR build/cmake
    CMAKE_ARGS -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF
)
