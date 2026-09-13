# sctp 1.0.20: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_autotools(sctp
    VERSION 1.0.20
    URL "https://github.com/sctp/lksctp-tools/archive/refs/tags/v1.0.20.tar.gz"
    SHA256 4b77098ca9beadca2dc4e351bc85909f98464dfe39ba319b5be6e52927d8e8e5
    ARCHIVES sctp
    PATCH_COMMAND autoreconf -fi
)
