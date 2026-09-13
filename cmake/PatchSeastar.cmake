file(READ "src/core/exception_hacks.cc" _content)
string(REGEX REPLACE
    "org = \\(dl_iterate_fn\\)dlsym \\(RTLD_NEXT, \"dl_iterate_phdr\"\\);"
    "org = &::dl_iterate_phdr;"
    _content "${_content}")
string(REGEX REPLACE
    "int dl_iterate_phdr\\(int \\(\\*callback\\) \\(struct dl_phdr_info \\*info, size_t size, void \\*data\\), void \\*data\\) \\{"
    "int seastar_dl_iterate_phdr_unused(int (*callback) (struct dl_phdr_info *info, size_t size, void *data), void *data) {"
    _content "${_content}")
file(WRITE "src/core/exception_hacks.cc" "${_content}")

# Static Boost.Test uses a different initialization callback from the shared
# library. Seastar installs its testing archive even with Seastar_TESTING=OFF.
file(READ "src/testing/entry_point.cc" _entry_point)
set(_old_init [=[static bool init_unit_test_suite() {
    auto&& ts = boost::unit_test::framework::master_test_suite();
    return global_test_runner().start(ts.argc, ts.argv);
}]=])
set(_new_init [=[#ifdef BOOST_TEST_ALTERNATIVE_INIT_API
static bool init_unit_test_suite() {
    auto&& ts = boost::unit_test::framework::master_test_suite();
    return global_test_runner().start(ts.argc, ts.argv);
}
#else
static boost::unit_test::test_suite* init_unit_test_suite(int argc, char** argv) {
    if (!global_test_runner().start(argc, argv)) {
        throw boost::unit_test::framework::setup_error("Unable to start Seastar test runner");
    }
    return nullptr;
}
#endif]=])
if(NOT _entry_point MATCHES "#ifdef BOOST_TEST_ALTERNATIVE_INIT_API")
    string(REPLACE "${_old_init}" "${_new_init}" _entry_point "${_entry_point}")
    file(WRITE "src/testing/entry_point.cc" "${_entry_point}")
endif()

# Include the AF_PACKET PMD in Seastar's static DPDK bundle. Upstream has a
# fixed archive list, so enabling the driver in Meson alone is insufficient.
function(spoe_seastar_replace old new)
    string(FIND "${_content}" "${old}" _position)
    if(_position EQUAL -1)
        message(FATAL_ERROR "Seastar patch no longer matches ${_file}: ${old}")
    endif()
    string(REPLACE "${old}" "${new}" _content "${_content}")
    set(_content "${_content}" PARENT_SCOPE)
endfunction()

set(_file "cmake/Finddpdk.cmake")
file(READ "${_file}" _content)
if(NOT _content MATCHES "  net_af_packet")
    spoe_seastar_replace("  net_bnxt" "  net_af_packet\n  net_bnxt")
    file(WRITE "${_file}" "${_content}")
endif()

# Let the application explicitly choose a kernel interface while retaining
# Seastar's native TCP/IP stack and DPDK PMD. Configure this before app.run();
# Seastar still owns EAL initialization and the selected CPU mask.
set(_file "include/seastar/core/dpdk_rte.hh")
file(READ "${_file}" _content)
if(NOT _content MATCHES "static std::optional<std::string> packet_interface;")
    spoe_seastar_replace("    static bool initialized;" [=[    static bool initialized;
    // SPOE: optional AF_PACKET interface, configured before EAL initialization.
    static std::optional<std::string> packet_interface;]=])
    file(WRITE "${_file}" "${_content}")
endif()

set(_file "src/core/dpdk_rte.cc")
file(READ "${_file}" _content)
if(NOT _content MATCHES "std::optional<std::string> eal::packet_interface;")
    spoe_seastar_replace("bool eal::initialized = false;" [=[bool eal::initialized = false;
std::optional<std::string> eal::packet_interface;]=])
    spoe_seastar_replace("    if (hugepages_path) {" [=[    if (packet_interface) {
        if (hugepages_path) {
            rte_exit(EXIT_FAILURE, "AF_PACKET cannot be combined with --hugepages\n");
        }
        // The software PMD copies packets and needs neither hugepages nor PCI.
        // One hardware queue also supports additional Seastar software queues.
        args.push_back(string2vector("--no-huge"));
        args.push_back(string2vector("--no-pci"));
        args.push_back(string2vector("--no-shconf"));
        args.push_back(string2vector("--iova-mode=va"));
        args.push_back(string2vector("--vdev=net_af_packet0,iface=" + *packet_interface + ",qpairs=1"));
        args.push_back(string2vector("-m"));
        args.push_back(string2vector("128"));
    } else if (hugepages_path) {]=])
    file(WRITE "${_file}" "${_content}")
endif()
