# hwloc 2.13.0: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_autotools(hwloc
    VERSION 2.13.0
    URL "https://github.com/open-mpi/hwloc/archive/refs/tags/hwloc-2.13.0.tar.gz"
    SHA256 f8f431cdc4d4f583d50983d5e1532c269ee5e39c3eef1dd37be6ea173b51359e
    ARCHIVES hwloc
    DEPENDS numa
    PATCH_COMMAND <SOURCE_DIR>/autogen.sh
    CONFIGURE_ARGS --disable-io --disable-libudev --disable-libxml2
        --disable-cairo --disable-pci --disable-cuda --disable-opencl
        --disable-nvml --disable-rsmi --disable-levelzero
        --disable-plugin-dlopen --disable-plugin-ltdl
)
