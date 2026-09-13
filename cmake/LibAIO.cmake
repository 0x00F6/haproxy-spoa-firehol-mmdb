# aio 0.3.113: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_custom(aio
    VERSION 0.3.113
    URL "https://releases.pagure.org/libaio/libaio-0.3.113.tar.gz"
    SHA256 2c44d1c5fd0d43752287c9ae1eb9c023f04ef848ea8d4aafa46e9aedb678200b
    ARCHIVES aio
    BUILD_IN_SOURCE
    CONFIGURE_COMMAND ""
    # Keep CFLAGS in SPOE_BUILD_ENV so libaio can append -I. and -fPIC.
    # A make command-line override suppresses those required flags.
    BUILD_COMMAND ${SPOE_BUILD_ENV} ${SPOE_MAKE} -C src -j${SPOE_DEPS_JOBS}
        libaio.a
    INSTALL_COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/src/libaio.a ${SPOE_SOURCE_PREFIX}/lib/libaio.a
        COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/src/libaio.h ${SPOE_SOURCE_PREFIX}/include/libaio.h
)
