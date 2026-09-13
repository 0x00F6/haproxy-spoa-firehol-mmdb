# zlib 1.3.1: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_custom(zlib
    VERSION 1.3.1
    URL "https://zlib.net/fossils/zlib-1.3.1.tar.gz"
    SHA256 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
    ARCHIVES z
    CONFIGURE_COMMAND ${SPOE_BUILD_ENV} <SOURCE_DIR>/configure --static --prefix=${SPOE_SOURCE_PREFIX}
)
