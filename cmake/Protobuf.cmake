# protobuf 21.12: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_cmake(protobuf
    VERSION 21.12
    URL "https://github.com/protocolbuffers/protobuf/releases/download/v21.12/protobuf-all-21.12.tar.gz"
    SHA256 2c6a36c7b5a55accae063667ef3c55f2642e67476d96d355ff0acb13dbb47f09
    ARCHIVES protobuf protoc
    DEPENDS zlib
    SOURCE_SUBDIR cmake
    CMAKE_ARGS -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF
        -Dprotobuf_WITH_ZLIB=ON -DZLIB_ROOT=${SPOE_SOURCE_PREFIX}
        -DZLIB_LIBRARY=${SPOE_SOURCE_PREFIX}/lib/libz.a
        -DZLIB_INCLUDE_DIR=${SPOE_SOURCE_PREFIX}/include
)
