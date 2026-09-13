# gmp 6.3.0: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_autotools(gmp
    VERSION 6.3.0
    URL "https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz"
    SHA256 a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898
    ARCHIVES gmp
    CONFIGURE_ARGS --disable-cxx
)
