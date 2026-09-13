# DPDK 23.07 builds shared targets explicitly even with default_library=static.
# Preserve its dependency/pkg-config metadata, but use the static archives for
# those references so no shared linker consumes our non-PIC source libraries.
function(spoe_dpdk_replace old new)
    string(FIND "${_content}" "${old}" _position)
    if(_position EQUAL -1)
        message(FATAL_ERROR "DPDK patch no longer matches ${_file}: ${old}")
    endif()
    string(REPLACE "${old}" "${new}" _content "${_content}")
    set(_content "${_content}" PARENT_SCOPE)
endfunction()

set(_file "lib/meson.build")
file(READ "${_file}" _content)
if(NOT _content MATCHES "# SPOE: static-only library")
    spoe_dpdk_replace(
        "    shared_lib = shared_library(libname,"
        "    # SPOE: static-only library\n    if get_option('default_library') == 'static'\n        shared_lib = static_lib\n    else\n    shared_lib = shared_library(libname,")
    spoe_dpdk_replace(
        "            install: true)\n    shared_dep = declare_dependency(link_with: shared_lib,"
        "            install: true)\n    endif\n    shared_dep = declare_dependency(link_with: shared_lib,")
    file(WRITE "${_file}" "${_content}")
endif()

set(_file "drivers/meson.build")
file(READ "${_file}" _content)
if(NOT _content MATCHES "# SPOE: static-only driver")
    spoe_dpdk_replace(
        "        shared_lib = shared_library(lib_name, sources,"
        "        # SPOE: static-only driver\n        if get_option('default_library') == 'static'\n            shared_lib = static_lib\n        else\n        shared_lib = shared_library(lib_name, sources,")
    spoe_dpdk_replace(
        "                install_dir: driver_install_path)"
        "                install_dir: driver_install_path)\n        endif")
    file(WRITE "${_file}" "${_content}")
endif()

# These optional features are unused by the selected Seastar PMDs. Disable
# probes that could discover host libraries or race another prefix installer.
# An empty optional dependency is Meson's standard not-found dependency.
set(_file "config/meson.build")
file(READ "${_file}" _content)
if(NOT _content MATCHES "# SPOE: deterministic optional dependencies")
    spoe_dpdk_replace(
        "fdt_dep = cc.find_library('fdt', required: false)"
        "# SPOE: deterministic optional dependencies\nfdt_dep = dependency('', required: false)")
    spoe_dpdk_replace(
        "libexecinfo = cc.find_library('execinfo', required: false)"
        "libexecinfo = dependency('', required: false)")
    spoe_dpdk_replace(
        "openssl_dep = dependency('openssl', required: false, method: 'pkg-config')"
        "openssl_dep = dependency('', required: false)")
    spoe_dpdk_replace(
        "pcap_dep = dependency('libpcap', required: false, method: 'pkg-config')"
        "pcap_dep = dependency('', required: false)")
    spoe_dpdk_replace(
        "    pcap_dep = cc.find_library(pcap_lib, required: false)"
        "    pcap_dep = dependency('', required: false)")
    file(WRITE "${_file}" "${_content}")
endif()
