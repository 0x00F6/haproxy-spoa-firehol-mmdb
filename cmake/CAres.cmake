# cares 1.34.5: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_cmake(cares
    VERSION 1.34.5
    URL "https://github.com/c-ares/c-ares/releases/download/v1.34.5/c-ares-1.34.5.tar.gz"
    SHA256 7d935790e9af081c25c495fd13c2cfcda4792983418e96358ef6e7320ee06346
    ARCHIVES cares
    CMAKE_ARGS -DCARES_STATIC=ON -DCARES_SHARED=OFF -DCARES_BUILD_TESTS=OFF -DCARES_BUILD_TOOLS=OFF
)
