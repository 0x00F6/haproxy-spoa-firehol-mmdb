# fmt 10.1.1: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_cmake(fmt
    VERSION 10.1.1
    URL "https://github.com/fmtlib/fmt/archive/refs/tags/10.1.1.tar.gz"
    SHA256 78b8c0a72b1c35e4443a7e308df52498252d1cefc2b08c9a97bc9ee6cfe61f8b
    ARCHIVES fmt
    CMAKE_ARGS -DFMT_TEST=OFF -DFMT_DOC=OFF
)
