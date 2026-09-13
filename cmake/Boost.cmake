# boost 1.87.0: built from source as static archives with -O3.
include_guard(GLOBAL)
file(WRITE "${CMAKE_BINARY_DIR}/boost-user-config.jam"
    "using gcc : spoa : \"${CMAKE_CXX_COMPILER}\" ;\n")
spoe_custom(boost
    VERSION 1.87.0
    URL "https://archives.boost.io/release/1.87.0/source/boost_1_87_0.tar.bz2"
    SHA256 af57be25cb4c4f4b413ed692fe378affb4352ea50fbe294a11ef548f4d527d89
    ARCHIVES boost_program_options boost_thread boost_atomic boost_chrono boost_date_time boost_filesystem boost_unit_test_framework
    BUILD_IN_SOURCE
    CONFIGURE_COMMAND ${SPOE_BUILD_ENV} <SOURCE_DIR>/bootstrap.sh --with-toolset=gcc
        --with-libraries=program_options,thread,atomic,chrono,date_time,filesystem,test
    BUILD_COMMAND ${SPOE_BUILD_ENV} <SOURCE_DIR>/b2 -j${SPOE_DEPS_JOBS}
        --user-config=${CMAKE_BINARY_DIR}/boost-user-config.jam
        toolset=gcc-spoa variant=release link=static runtime-link=static threading=multi
        cflags=-O3 cxxflags=-O3 --layout=system --prefix=${SPOE_SOURCE_PREFIX}
        --libdir=${SPOE_SOURCE_PREFIX}/lib install
    INSTALL_COMMAND ""
)
