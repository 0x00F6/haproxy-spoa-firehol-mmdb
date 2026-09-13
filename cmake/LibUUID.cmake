# uuid 2.40.4: static -O3 library.
include_guard(GLOBAL)
spoe_autotools(uuid
    VERSION 2.40.4
    URL "https://www.kernel.org/pub/linux/utils/util-linux/v2.40/util-linux-2.40.4.tar.xz"
    SHA256 5c1daf733b04e9859afdc3bd87cc481180ee0f88b5c0946b16fdec931975fb79
    ARCHIVES uuid
    CONFIGURE_ARGS --disable-all-programs --enable-libuuid --disable-nls
)
