# tasn1 4.20.0: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_autotools(tasn1
    VERSION 4.20.0
    URL "https://ftp.gnu.org/gnu/libtasn1/libtasn1-4.20.0.tar.gz"
    SHA256 92e0e3bd4c02d4aeee76036b2ddd83f0c732ba4cda5cb71d583272b23587a76c
    ARCHIVES tasn1
    CONFIGURE_ARGS --disable-doc
)
