# ---------------------------------------------------------------------------
# p-ranav/argparse - a single-header, header-only C++ argument parser.
# Fetched from source and exposed as the INTERFACE target argparse::argparse.
# Header-only: nothing is compiled, so it is trivially usable in a fully
# static (-O3) build. Only the header is installed.
# ---------------------------------------------------------------------------

include(FetchContent)

set(ARGPARSE_VERSION "v3.2")
set(ARGPARSE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ARGPARSE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ARGPARSE_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ARGPARSE_INSTALL ON CACHE BOOL "" FORCE)

FetchContent_Declare(argparse
    GIT_REPOSITORY https://github.com/p-ranav/argparse.git
    GIT_TAG        ${ARGPARSE_VERSION}
    GIT_SHALLOW    TRUE
    )

FetchContent_MakeAvailable(argparse)

# argparse's own CMake defines argparse::argparse as an INTERFACE library, so
# nothing extra is needed here; the target is ready to be linked.
