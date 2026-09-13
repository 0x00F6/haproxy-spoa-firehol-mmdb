# gnutls 3.8.12: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_autotools(gnutls
    VERSION 3.8.12
    URL "https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/gnutls-3.8.12.tar.xz"
    SHA256 a7b341421bfd459acf7a374ca4af3b9e06608dcd7bd792b2bf470bea012b8e51
    ARCHIVES gnutls
    DEPENDS nettle gmp tasn1 unistring zlib
    CONFIGURE_ARGS --disable-tools --disable-tests --disable-doc --disable-cxx
        --disable-libdane --disable-rpath --without-tpm2 --without-p11-kit --without-idn --without-brotli
        --without-zstd --with-zlib=link --without-included-unistring
        --with-libunistring-prefix=${SPOE_SOURCE_PREFIX}
)
