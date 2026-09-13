# DPDK 23.07.0: use the exact submodule revision shipped with Seastar 25.05.0.
include_guard(GLOBAL)
find_program(SPOE_MESON NAMES meson REQUIRED)
find_program(SPOE_NINJA NAMES ninja REQUIRED)

# Keep the PMDs required by Seastar's Finddpdk.cmake, plus AF_PACKET for
# container benchmarks, without drivers that need extra system libraries.
set(_dpdk_drivers bus/pci bus/vdev mempool/ring
    net/af_packet net/bnxt net/cxgbe net/e1000 net/ena net/enic net/i40e net/ixgbe
    net/nfp net/qede net/ring net/sfc net/vmxnet3)
set(_dpdk_archives rte_bus_pci rte_bus_vdev rte_cfgfile rte_cmdline
    rte_cryptodev rte_eal rte_ethdev rte_hash rte_kvargs rte_mbuf rte_mempool
    rte_mempool_ring rte_net rte_net_af_packet rte_net_bnxt rte_net_cxgbe rte_net_e1000
    rte_net_ena rte_net_enic rte_net_i40e rte_net_ixgbe rte_net_nfp
    rte_net_qede rte_net_ring rte_net_sfc rte_net_vmxnet3 rte_pci rte_rcu
    rte_ring rte_security rte_telemetry rte_timer)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "amd64|x86_64|aarch64")
    list(APPEND _dpdk_drivers common/sfc_efx)
    list(APPEND _dpdk_archives rte_common_sfc_efx)
endif()
list(JOIN _dpdk_drivers "," _dpdk_drivers)

spoe_custom(dpdk
    VERSION 23.07.0-cafaa3cf4575
    URL "https://github.com/scylladb/dpdk/archive/cafaa3cf457584de3c4de1998fc67bd03b26f2f7.tar.gz"
    SHA256 36394132d3b063519af0437489dd8719fe614e7a625f49c4df0ca86e9a1275c1
    ARCHIVES ${_dpdk_archives}
    DEPENDS numa
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/PatchDPDK.cmake
    # --reconfigure also handles the first setup (Meson >= 1.1), and applies
    # changed recipe options when ExternalProject invalidates its stamps.
    CONFIGURE_COMMAND ${SPOE_BUILD_ENV} ${SPOE_MESON} setup --reconfigure
        <BINARY_DIR> <SOURCE_DIR> --prefix=${SPOE_SOURCE_PREFIX}
        --libdir=lib --includedir=include/dpdk --buildtype=release
        --default-library=static --wrap-mode=nofallback
        -Ddeveloper_mode=disabled -Denable_docs=false -Ddisable_apps=*
        -Dtests=false -Dexamples= -Denable_kmods=false
        -Dmbuf_refcnt_atomic=false -Dmax_memseg_lists=8192
        -Ddisable_libs=flow_classify,kni,jobstats,power,port,table,pipeline,member
        -Denable_drivers=${_dpdk_drivers} -Dcpu_instruction_set=native
    BUILD_COMMAND ${SPOE_BUILD_ENV} ${SPOE_NINJA} -C <BINARY_DIR> -j${SPOE_DEPS_JOBS}
    # Install archives, headers and pkg-config metadata; skip runtime helpers
    # and the upstream post-install script that symlinks shared PMDs.
    INSTALL_COMMAND ${SPOE_BUILD_ENV} ${SPOE_MESON} install -C <BINARY_DIR>
        --no-rebuild --tags devel
)
unset(_dpdk_drivers)
unset(_dpdk_archives)
