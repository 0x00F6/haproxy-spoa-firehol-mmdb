# snappy 1.2.2: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_cmake(snappy
    VERSION 1.2.2
    URL "https://github.com/google/snappy/archive/refs/tags/1.2.2.tar.gz"
    SHA256 90f74bc1fbf78a6c56b3c4a082a05103b3a56bb17bca1a27e052ea11723292dc
    ARCHIVES snappy
    CMAKE_ARGS -DSNAPPY_BUILD_TESTS=OFF -DSNAPPY_BUILD_BENCHMARKS=OFF
)
