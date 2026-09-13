# uring 2.9: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_custom(uring
    VERSION 2.9
    URL "https://github.com/axboe/liburing/archive/refs/tags/liburing-2.9.tar.gz"
    SHA256 897b1153b55543e8b92a5a3eb9b906537a5fedcf8afaf241f8b8787940c79f8d
    ARCHIVES uring
    BUILD_IN_SOURCE
    CONFIGURE_COMMAND ${SPOE_BUILD_ENV} <SOURCE_DIR>/configure
        --prefix=${SPOE_SOURCE_PREFIX} --libdir=${SPOE_SOURCE_PREFIX}/lib
        --libdevdir=${SPOE_SOURCE_PREFIX}/lib
    BUILD_COMMAND ${SPOE_BUILD_ENV} ${SPOE_MAKE} -j${SPOE_DEPS_JOBS} -C src ENABLE_SHARED=0
    INSTALL_COMMAND ${SPOE_BUILD_ENV} ${SPOE_MAKE} ENABLE_SHARED=0 install
)
