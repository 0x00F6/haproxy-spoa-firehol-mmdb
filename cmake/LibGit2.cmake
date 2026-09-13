spoe_cmake(git2
    VERSION 1.9.7
    URL https://github.com/libgit2/libgit2/archive/refs/tags/v1.9.7.tar.gz
    SHA256 1a4fbe7589e814777ae76b64734ad80f4ecad22cd33a22682a2aaea4ae5375e7
    ARCHIVES git2
    DEPENDS zlib openssl
    CMAKE_ARGS
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_TESTS=OFF
        -DBUILD_CLI=OFF
        -DUSE_SSH=OFF
        -DUSE_HTTPS=OpenSSL
        -DUSE_BUNDLED_ZLIB=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)
