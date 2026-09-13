# Match the public headers installed by xfsprogs, without building its tools.
file(INSTALL DESTINATION "${PREFIX}/include/xfs" TYPE FILE FILES
    "${SOURCE_DIR}/include/handle.h"
    "${SOURCE_DIR}/include/jdm.h"
    "${SOURCE_DIR}/include/linux.h"
    "${SOURCE_DIR}/include/xfs.h"
    "${SOURCE_DIR}/include/xqm.h"
    "${SOURCE_DIR}/include/xfs_fs_compat.h"
    "${SOURCE_DIR}/include/xfs_arch.h"
    "${SOURCE_DIR}/libxfs/xfs_fs.h"
    "${SOURCE_DIR}/libxfs/xfs_types.h"
    "${SOURCE_DIR}/libxfs/xfs_da_format.h"
    "${SOURCE_DIR}/libxfs/xfs_format.h"
    "${SOURCE_DIR}/libxfs/xfs_log_format.h")
