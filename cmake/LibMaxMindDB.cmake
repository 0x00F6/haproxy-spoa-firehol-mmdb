# maxminddb 1.14.0: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_autotools(maxminddb
    VERSION 1.14.0
    URL "https://github.com/maxmind/libmaxminddb/releases/download/1.14.0/libmaxminddb-1.14.0.tar.gz"
    SHA256 65ff92382c71ef6634b8c13e278651a2efa68f1de28ef3c31fc32369fa0bb3e3
    ARCHIVES maxminddb
    CONFIGURE_ARGS --disable-tests
)
