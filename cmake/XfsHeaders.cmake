# xfs 6.12.0: public headers only; Seastar does not link libxfs.
include_guard(GLOBAL)
spoe_custom(xfs
    VERSION 6.12.0
    URL "https://www.kernel.org/pub/linux/utils/fs/xfs/xfsprogs/xfsprogs-6.12.0.tar.xz"
    SHA256 0832407247db791cc70def96e7e254bd6edf043dc84a80a62f3ccd6e3dffd329
    DEPENDS uuid
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E true
    BUILD_COMMAND ${CMAKE_COMMAND} -E true
    INSTALL_COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=<SOURCE_DIR>
        -DPREFIX=${SPOE_SOURCE_PREFIX} -P ${CMAKE_CURRENT_LIST_DIR}/InstallXfsHeaders.cmake
)
