if(NOT DEFINED PROGRAM OR NOT EXISTS "${PROGRAM}")
    message(FATAL_ERROR "PROGRAM must name the built SPOA agent")
endif()

execute_process(
    COMMAND "${PROGRAM}" --help
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)

set(log "${output}\n${error}")
if(log MATCHES "Logger '[^']+' registered twice")
    message(FATAL_ERROR "SPOA registered a logger twice:\n${log}")
endif()

if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "SPOA failed during startup (result: ${result}):\n${log}")
endif()

# Seastar reads configuration before --version exits, so these cases exercise
# the real parser without initializing reactors, opening sockets, or using DPDK.
function(expect_config_startup expected_result expected_message)
    execute_process(
        COMMAND "${PROGRAM}" ${ARGN} --version
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 10)
    set(log "${output}\n${error}")
    if(NOT "${result}" STREQUAL "${expected_result}")
        message(FATAL_ERROR
            "Config arguments ${ARGN} returned ${result}, expected ${expected_result}:\n${log}")
    endif()
    string(FIND "${log}" "${expected_message}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR
            "Config arguments ${ARGN} did not report '${expected_message}':\n${log}")
    endif()
endfunction()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef config_test_id)
set(config_test_dir "${CMAKE_CURRENT_BINARY_DIR}/startup-config-${config_test_id}")
file(MAKE_DIRECTORY "${config_test_dir}")
set(seastar_conf "${config_test_dir}/seastar custom.conf")
set(io_conf "${config_test_dir}/io custom.conf")
file(WRITE "${seastar_conf}" "network-stack=posix\nsmp=1\n")
file(WRITE "${io_conf}" "task-quota-ms=0.5\n")
expect_config_startup(0 ""
    --seastar-conf "${seastar_conf}" --io-conf "${io_conf}")

# Every explicit path must be checked; directories must also fail even when
# the test runs as root (unlike a chmod-based unreadability test).
foreach(option IN ITEMS seastar-conf io-conf)
    foreach(invalid_path IN ITEMS
            "${config_test_dir}/missing.conf" "${config_test_dir}")
        if(option STREQUAL "seastar-conf")
            expect_config_startup(2 "${invalid_path}"
                --seastar-conf "${invalid_path}" --io-conf "${io_conf}")
        else()
            expect_config_startup(2 "${invalid_path}"
                --seastar-conf "${seastar_conf}" --io-conf "${invalid_path}")
        endif()
    endforeach()
endforeach()

# Invalid typed values prove that both selected files are actually parsed.
# Explicit command-line values must take precedence over those file values.
file(WRITE "${seastar_conf}" "smp=invalid-shard-count\n")
expect_config_startup(2 "smp"
    --seastar-conf "${seastar_conf}" --io-conf "${io_conf}")
file(WRITE "${io_conf}" "task-quota-ms=invalid-task-quota\n")
expect_config_startup(2 "task-quota-ms"
    --seastar-conf "${seastar_conf}" --io-conf "${io_conf}" --smp 1)
expect_config_startup(0 ""
    --seastar-conf "${seastar_conf}" --io-conf "${io_conf}"
    --smp 1 --task-quota-ms 0.5)

file(REMOVE_RECURSE "${config_test_dir}")
