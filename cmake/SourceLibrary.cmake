# Common policy for all third-party source builds. No installation into /usr.
include_guard(GLOBAL)
include(ExternalProject)
find_program(SPOE_MAKE NAMES gmake make REQUIRED)
# Keep IDE and terminal PATH differences from invalidating every build stamp.
set(SPOE_DEPENDENCY_TOOL_PATH "$ENV{PATH}" CACHE STRING
    "Search path for dependency build tools")
set(SPOE_BUILD_ENV ${CMAKE_COMMAND} -E env --unset=MAKEFLAGS
    "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
    "CFLAGS=-O3 -std=gnu11" "CXXFLAGS=-O3"
    "CPPFLAGS=-I${SPOE_SOURCE_PREFIX}/include"
    "LDFLAGS=-L${SPOE_SOURCE_PREFIX}/lib"
    "PKG_CONFIG_PATH=${SPOE_SOURCE_PREFIX}/lib/pkgconfig"
    "PKG_CONFIG_LIBDIR=${SPOE_SOURCE_PREFIX}/lib/pkgconfig"
    "PATH=${SPOE_SOURCE_PREFIX}/bin:${SPOE_DEPENDENCY_TOOL_PATH}")
file(MAKE_DIRECTORY "${SPOE_SOURCE_PREFIX}/include" "${SPOE_SOURCE_PREFIX}/lib")

function(spoe_source name kind)
    cmake_parse_arguments(P "BUILD_IN_SOURCE" "VERSION;URL;SHA256;SOURCE_SUBDIR"
        "ARCHIVES;DEPENDS;CONFIGURE_ARGS;CMAKE_ARGS;CONFIGURE_COMMAND;BUILD_COMMAND;INSTALL_COMMAND;PATCH_COMMAND" ${ARGN})
    if(P_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown source options for ${name}: ${P_UNPARSED_ARGUMENTS}")
    endif()
    set_property(GLOBAL APPEND PROPERTY SPOE_CACHE_LIBRARIES "${name} ${P_VERSION}")
    string(REGEX MATCH "\\.tar\\.(gz|xz|bz2)$" _suffix "${P_URL}")
    set(_archive "${SPOE_DEPS_CACHE_DIR}/downloads/${name}-${P_VERSION}${_suffix}")
    # Keep ExternalProject's URL stable across the first and subsequent builds.
    # Download atomically so concurrent configurations cannot see a partial file.
    file(MAKE_DIRECTORY "${SPOE_DEPS_CACHE_DIR}/downloads")
    if(NOT EXISTS "${_archive}")
        file(LOCK "${_archive}.lock" GUARD FUNCTION TIMEOUT 300)
        if(NOT EXISTS "${_archive}")
            message(STATUS "Downloading ${name} ${P_VERSION}")
            file(DOWNLOAD "${P_URL}" "${_archive}.tmp"
                EXPECTED_HASH SHA256=${P_SHA256} TLS_VERIFY ON
                STATUS _download_status)
            list(GET _download_status 0 _download_code)
            if(NOT _download_code EQUAL 0)
                file(REMOVE "${_archive}.tmp")
                message(FATAL_ERROR "Download failed for ${name}: ${_download_status}")
            endif()
            file(RENAME "${_archive}.tmp" "${_archive}")
        endif()
    endif()
    set(_url "${_archive}")
    set(_byproducts)
    foreach(_lib IN LISTS P_ARCHIVES)
        list(APPEND _byproducts "${SPOE_SOURCE_PREFIX}/lib/lib${_lib}.a")
    endforeach()
    set(_commands)
    if(kind STREQUAL "cmake")
        set(_cmake_args
                -DCMAKE_POLICY_VERSION_MINIMUM=3.5
                -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
                -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                "-DCMAKE_C_FLAGS=-O3"
                "-DCMAKE_CXX_FLAGS=-O3"
                "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG"
                "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG"
                "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O3 -g -DNDEBUG"
                "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O3 -g -DNDEBUG"
                -DCMAKE_POSITION_INDEPENDENT_CODE=ON
                -DCMAKE_INSTALL_PREFIX=${SPOE_SOURCE_PREFIX}
                -DCMAKE_INSTALL_LIBDIR=lib
                -DCMAKE_PREFIX_PATH=${SPOE_SOURCE_PREFIX}
                -DCMAKE_INSTALL_RPATH=
                -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
                ${P_CMAKE_ARGS})
        list(APPEND _commands
            CONFIGURE_COMMAND ${SPOE_BUILD_ENV} ${CMAKE_COMMAND} <SOURCE_DIR>/${P_SOURCE_SUBDIR}
                -G Ninja ${_cmake_args}
            BUILD_COMMAND ${SPOE_BUILD_ENV} ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${SPOE_DEPS_JOBS}
            INSTALL_COMMAND ${SPOE_BUILD_ENV} ${CMAKE_COMMAND} --install <BINARY_DIR>)
    else()
        if(kind STREQUAL "autotools")
            set(P_CONFIGURE_COMMAND ${SPOE_BUILD_ENV} <SOURCE_DIR>/configure
                --prefix=${SPOE_SOURCE_PREFIX} --libdir=${SPOE_SOURCE_PREFIX}/lib
                --disable-shared --enable-static ${P_CONFIGURE_ARGS})
        endif()
        if(NOT DEFINED P_BUILD_COMMAND)
            set(P_BUILD_COMMAND ${SPOE_BUILD_ENV} ${SPOE_MAKE} -j${SPOE_DEPS_JOBS})
        endif()
        if(NOT "INSTALL_COMMAND" IN_LIST P_KEYWORDS_MISSING_VALUES AND NOT DEFINED P_INSTALL_COMMAND)
            set(P_INSTALL_COMMAND ${SPOE_BUILD_ENV} ${SPOE_MAKE} install)
        endif()
        foreach(_step CONFIGURE BUILD INSTALL)
            if(NOT P_${_step}_COMMAND)
                set(P_${_step}_COMMAND ${CMAKE_COMMAND} -E true)
            endif()
        endforeach()
        list(APPEND _commands CONFIGURE_COMMAND ${P_CONFIGURE_COMMAND}
            BUILD_COMMAND ${P_BUILD_COMMAND} INSTALL_COMMAND ${P_INSTALL_COMMAND})
    endif()
    if(P_BUILD_IN_SOURCE)
        set(_build_dir BUILD_IN_SOURCE TRUE)
    else()
        set(_build_dir BINARY_DIR "${CMAKE_BINARY_DIR}/${name}/build")
    endif()
    ExternalProject_Add(${name}
        URL "${_url}" URL_HASH SHA256=${P_SHA256}
        DOWNLOAD_NAME "${name}-${P_VERSION}${_suffix}"
        DOWNLOAD_DIR "${SPOE_DEPS_CACHE_DIR}/downloads"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE TLS_VERIFY TRUE
        PREFIX "${CMAKE_BINARY_DIR}/${name}"
        SOURCE_DIR "${CMAKE_BINARY_DIR}/${name}/source"
        STAMP_DIR "${CMAKE_BINARY_DIR}/${name}/stamp"
        ${_build_dir}
        DEPENDS ${P_DEPENDS}
        PATCH_COMMAND ${P_PATCH_COMMAND}
        ${_commands}
        BUILD_BYPRODUCTS ${_byproducts}
        LOG_DOWNLOAD ON LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON
        LOG_OUTPUT_ON_FAILURE ON)
    foreach(_step PATCH INSTALL)
        string(TOLOWER "${_step}" _step_name)
        foreach(_arg IN LISTS P_${_step}_COMMAND)
            if(_arg MATCHES "\\.cmake$" AND EXISTS "${_arg}")
                ExternalProject_Add_StepDependencies(${name} ${_step_name} "${_arg}")
            endif()
        endforeach()
    endforeach()
endfunction()

function(spoe_cmake name)
    spoe_source(${name} cmake ${ARGN})
endfunction()
function(spoe_autotools name)
    spoe_source(${name} autotools ${ARGN})
endfunction()
function(spoe_custom name)
    spoe_source(${name} custom ${ARGN})
endfunction()
