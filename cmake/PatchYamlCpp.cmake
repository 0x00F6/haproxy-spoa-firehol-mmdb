# yaml-cpp 0.8.0 relied on an indirect <cstdint> include removed by GCC 15.
file(READ "src/emitterutils.cpp" _source)
if(NOT _source MATCHES "#include <cstdint>")
    file(WRITE "src/emitterutils.cpp" "#include <cstdint>\n${_source}")
endif()
