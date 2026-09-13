BUILD_DIR ?= build
CC := gcc-15
CXX := g++-15
CXXFLAGS ?= -fcoroutines
CMAKE_BUILD_TYPE ?= Release
BUILD_FULL_STATIC ?= ON
BUILD_PROFILE ?= OFF
SPOE_DEPS_JOBS ?= 2
# Modules built concurrently; empty derives it from CPU count and RAM.
SPOA_DEPS_PARALLEL ?=
BENCH_ARGS ?= --duration 30 --warmup 5 --connections 300
BENCH_DPDK_INTERFACE ?= spoa-dpdk
BENCH_DPDK_ARGS ?= $(filter-out --dpdk-interface%,$(BENCH_ARGS)) --dpdk-interface $(BENCH_DPDK_INTERFACE)
BENCH_FLAMEGRAPH ?=
BENCH_IMAGE ?= haproxy-spoa-firehol-mmdb-bench:local
BENCH_OUTPUT_DIR ?= $(abspath $(BUILD_DIR)/bench)

# Capture the start before running prerequisites, including CMake configure.
BUILD_STARTED_AT := $(shell date +%s)

# Ninja rewrites one status line on a terminal, so a long build shows nothing.
# TERM=dumb logs every step on its own line; NINJA_STATUS adds elapsed seconds.
# V=1 also prints the full compiler and linker command lines.
configure build: export TERM = dumb
configure build: export NINJA_STATUS = [%f/%t %es] 
CMAKE_BUILD_FLAGS = $(if $(filter-out 0,$(V)),--verbose)

CMAKE_ARGS = -DCMAKE_CXX_COMPILER=$(CXX) \
              -DCMAKE_CXX_FLAGS="$(CXXFLAGS)" \
              -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
              -DBUILD_FULL_STATIC=$(BUILD_FULL_STATIC) \
              -DSPOE_PROFILE=$(BUILD_PROFILE) \
              -DSPOE_DEPS_JOBS=$(SPOE_DEPS_JOBS) \
              -DSPOA_DEPS_PARALLEL=$(SPOA_DEPS_PARALLEL)

.PHONY: all configure build run test bench bench-dpdk flamegraph flamegraph-dpdk bench-container install clean fmt check fmt-check up down logs

firehol.mmdb: firehol.mmdb.tar.gz
	tar -xzf $<

all: build

configure:
	cmake -S . -B $(BUILD_DIR) -G Ninja $(CMAKE_ARGS)

build: configure
	cmake --build $(BUILD_DIR) $(CMAKE_BUILD_FLAGS)
	@set -eu; \
	duration=$$(( $$(date +%s) - $(BUILD_STARTED_AT) )); \
	binary=$$(realpath -- "$(BUILD_DIR)/haproxy-spoa-firehol-mmdb"); \
	version=$$("$$binary" --version); \
	size=$$(stat -c %s -- "$$binary"); \
	human_size=$$(numfmt --to=iec-i --suffix=B "$$size"); \
	checksum=$$(sha256sum -- "$$binary"); \
	checksum=$${checksum%% *}; \
	program_headers=$$(LC_ALL=C readelf -lW -- "$$binary"); \
	dynamic_section=$$(LC_ALL=C readelf -dW -- "$$binary"); \
	if printf '%s\n%s\n' "$$program_headers" "$$dynamic_section" | grep -Eq 'INTERP|\(NEEDED\)'; then \
	    linkage='🔗 Dynamic'; \
	else \
	    linkage='📦 Static'; \
	fi; \
	printf '\n%s\n' '━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━'; \
	printf '%s\n' '🎉 Build complete!'; \
	printf '⏱️  Duration   : %02dh %02dm %02ds\n' "$$((duration / 3600))" "$$((duration / 60 % 60))" "$$((duration % 60))"; \
	printf '🚀 Binary     : %s\n' "$$binary"; \
	printf '🏷️  Version    : %s\n' "$$version"; \
	printf '📏 Size       : %s (%s bytes)\n' "$$human_size" "$$size"; \
	printf '⚙️  Linkage    : %s\n' "$$linkage"; \
	printf '🔐 SHA-256    : %s\n' "$$checksum"; \
	printf '%s\n\n' '━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━'

run: build firehol.mmdb
	$(BUILD_DIR)/haproxy-spoa-firehol-mmdb $(ARGS)

test: build firehol.mmdb
	ctest --test-dir $(BUILD_DIR) -j$$(nproc) --output-on-failure

bench: firehol.mmdb
	@set -euo pipefail; \
	docker build -f bench.Dockerfile -t "$(BENCH_IMAGE)" .; \
	mkdir -p "$(BENCH_OUTPUT_DIR)"; \
	cp -a config/seastar.conf config/seastar.conf.bench.bak; \
	restore_conf() { \
	    status=$$?; \
	    trap - EXIT INT TERM HUP; \
	    if [ -f config/seastar.conf.bench.bak ]; then \
	        mv -f config/seastar.conf.bench.bak config/seastar.conf; \
	        printf '🔄 Restored original config/seastar.conf\n'; \
	    fi; \
	    exit $$status; \
	}; \
	trap restore_conf EXIT INT TERM HUP; \
	tune_conf() { \
	    key="$$1"; val="$$2"; file="config/seastar.conf"; \
	    if grep -Eq "^[# ]*$${key}=" "$$file"; then \
	        sed -E -i "s|^([# ]*$${key}=)[^ #]+(.*)|\1$${val}\2|" "$$file"; \
	    else \
	        printf '%s=%s\n' "$$key" "$$val" >> "$$file"; \
	    fi; \
	}; \
	printf '⚡ Tuning config/seastar.conf for benchmark...\n'; \
	tune_conf "default-log-level" "error"; \
	tune_conf "mbind" "1"; \
	tune_conf "abort-on-seastar-bad-alloc" "1"; \
	tune_conf "reactor-backend" "io_uring"; \
	tune_conf "reserve-memory" "8G"; \
	tune_conf "thread-affinity" "1"; \
	docker run --rm --init \
	    --cap-add NET_ADMIN --cap-add PERFMON --cap-add SYS_PTRACE \
	    --security-opt seccomp=unconfined \
	    --mount "type=bind,src=$(CURDIR),dst=$(CURDIR)" \
	    --mount "type=bind,src=$(abspath $(BENCH_OUTPUT_DIR)),dst=/results" \
	    --env BENCH_HOST_OUTPUT_DIR="$(abspath $(BENCH_OUTPUT_DIR))" \
	    --env SPOA_BENCH_UID="$$(id -u)" --env SPOA_BENCH_GID="$$(id -g)" \
	    --workdir "$(CURDIR)" \
	    --env HOME=/tmp --env BUILD_FULL_STATIC="$(BUILD_FULL_STATIC)" \
	    --env BUILD_DIR="$(BUILD_DIR)" \
	    --env CMAKE_BUILD_TYPE="$(CMAKE_BUILD_TYPE)" \
	    --env MMDB_PATH --env DROP_BY_CATEGORY \
	    --env CXXFLAGS="$(CXXFLAGS)" --env SPOE_DEPS_JOBS="$(SPOE_DEPS_JOBS)" \
	    --env SPOA_DEPS_PARALLEL="$(SPOA_DEPS_PARALLEL)" \
	    --env BENCH_ARGS="$(BENCH_ARGS)" \
	    --env BENCH_FLAMEGRAPH="$(BENCH_FLAMEGRAPH)" \
	    "$(BENCH_IMAGE)"
	@printf '\n🔥 Benchmark artifacts: %s\n' "$(BENCH_OUTPUT_DIR)"

# Run the benchmark and record a CPU flamegraph. Equivalent to
# `make bench BENCH_FLAMEGRAPH=1`.
flamegraph: firehol.mmdb
	$(MAKE) bench BENCH_FLAMEGRAPH=1

# Run the benchmark with Seastar DPDK AF_PACKET enabled.
bench-dpdk: firehol.mmdb
	$(MAKE) bench BENCH_ARGS="$(BENCH_DPDK_ARGS)"

# Run the DPDK benchmark and record a CPU flamegraph.
flamegraph-dpdk: firehol.mmdb
	$(MAKE) bench-dpdk BENCH_FLAMEGRAPH=1

# ---------------------------------------------------------------------------
# `bench-container` is the entrypoint target run inside the bench image. It
# contains the former scripts/bench-container.sh logic: verify profiling
# access, build a -O3 profiled binary, then run benchmark_spoa while
# teeing output to a log under /results.
# ---------------------------------------------------------------------------
bench-container:
	@set -euo pipefail; \
	log="$$(mktemp /results/benchmark-XXXXXXXX.log)"; \
	printf 'Benchmark log: %s\n' "$$log"; \
	perf stat -e cpu-clock:u -- true; \
	make build BUILD_DIR="$(BUILD_DIR)" BUILD_PROFILE=ON; \
	binary="$$(realpath "$(BUILD_DIR)/haproxy-spoa-firehol-mmdb")"; \
	printf 'Benchmark output directory: %s\n' "$${BENCH_HOST_OUTPUT_DIR:-/results}"; \
	BENCH_FLAMEGRAPH=$${BENCH_FLAMEGRAPH:-}; \
	flamegraph=$$([ -n "$$BENCH_FLAMEGRAPH" ] && printf '%s' '--flamegraph /results' || true); \
	$(BUILD_DIR)/benchmark_spoa --binary "$$binary" \
	    --seastar-conf config/seastar.conf --io-conf config/io.conf \
	    $$flamegraph $(BENCH_ARGS) 2>&1 | tee "$$log"

clean:
	@if [ -f config/seastar.conf.bench.bak ]; then mv -f config/seastar.conf.bench.bak config/seastar.conf; fi
	rm -rf $(BUILD_DIR) cmake-build-* .cache firehol-blocklist-ipsets config/seastar.conf.bench.bak

# croncpp.h is a vendored third-party header and keeps its upstream formatting.
fmt:
	find src/ \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) \
	  -not -name croncpp.h -exec clang-format --sort-includes -i {} +

check:
	find src/ \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) \
	  -not -name croncpp.h -exec clang-format --sort-includes --dry-run -Werror {} +

fmt-check: check

up:
	docker compose up --build

down:
	docker compose down

logs:
	docker compose logs -f
