# openssl 3.5.8: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_custom(openssl
    VERSION 3.5.8
    URL "https://www.openssl.org/source/openssl-3.5.8.tar.gz"
    SHA256 a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2
    ARCHIVES crypto ssl
    CONFIGURE_COMMAND ${SPOE_BUILD_ENV} <SOURCE_DIR>/Configure
        --prefix=${SPOE_SOURCE_PREFIX} --libdir=lib no-shared no-module no-dso no-tests -O3
    INSTALL_COMMAND ${SPOE_BUILD_ENV} ${SPOE_MAKE} install_sw
)
