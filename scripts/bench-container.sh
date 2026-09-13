#!/bin/bash
set -euo pipefail

run_benchmark() {
    # Fail before compiling if the runtime did not grant profiling access.
    perf stat -e cpu-clock:u -- true
    make build BUILD_DIR="${BENCH_BUILD_DIR:-.cache/bench-build}" BUILD_PROFILE=ON
    local binary
    binary="$(realpath "${BENCH_BUILD_DIR:-.cache/bench-build}/haproxy-spoa-firehol-mmdb")"
    echo "Benchmark output directory: ${BENCH_HOST_OUTPUT_DIR:-/results}"
    "${BENCH_BUILD_DIR:-.cache/bench-build}/benchmark_spoa" --binary "$binary" --flamegraph /results "$@"
}

log="$(mktemp /results/benchmark-XXXXXXXX.log)"
run_benchmark "$@" 2>&1 | tee "$log"
