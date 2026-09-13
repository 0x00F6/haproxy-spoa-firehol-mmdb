# yaml 0.8.0: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_cmake(yaml
    VERSION 0.8.0
    URL "https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz"
    SHA256 fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16
    ARCHIVES yaml-cpp
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/PatchYamlCpp.cmake
    CMAKE_ARGS -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF -DYAML_CPP_BUILD_CONTRIB=OFF -DYAML_BUILD_SHARED_LIBS=OFF
)
