# zstd 1.5.7: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_cmake(zstd
    VERSION 1.5.7
    URL "https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz"
    SHA256 eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    ARCHIVES zstd
    SOURCE_SUBDIR build/cmake
    CMAKE_ARGS -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
)
