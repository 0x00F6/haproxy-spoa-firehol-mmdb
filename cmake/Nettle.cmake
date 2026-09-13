# nettle 3.10.2: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_autotools(nettle
    VERSION 3.10.2
    URL "https://ftp.gnu.org/gnu/nettle/nettle-3.10.2.tar.gz"
    SHA256 fe9ff51cb1f2abb5e65a6b8c10a92da0ab5ab6eaf26e7fc2b675c45f1fb519b5
    ARCHIVES nettle hogweed
    DEPENDS gmp
    CONFIGURE_ARGS --disable-documentation --with-lib-path=${SPOE_SOURCE_PREFIX}/lib --with-include-path=${SPOE_SOURCE_PREFIX}/include
)
