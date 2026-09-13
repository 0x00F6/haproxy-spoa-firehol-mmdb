# syntax=docker/dockerfile:1
FROM ubuntu:26.04

ARG DEBIAN_FRONTEND=noninteractive
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc-15 g++-15 cmake meson ninja-build pkg-config diffutils make \
        python3 python3-pip python3-venv python3-yaml python3-pyelftools \
        curl ca-certificates git ragel valgrind gettext autoconf automake libtool \
        perl m4 texinfo gawk bison flex systemtap-sdt-dev \
        linux-perf binutils iproute2 ethtool util-linux \
    && rm -rf /var/lib/apt/lists/* \
    && perf --version

# Non-root Docker processes lose effective capabilities across exec. Grant only
# the profiling executable the capabilities allowed by docker run's bounding set.
RUN apt-get update && apt-get install -y --no-install-recommends libcap2-bin \
    && rm -rf /var/lib/apt/lists/* \
    && setcap cap_perfmon,cap_sys_ptrace=ep /usr/bin/perf

ARG FLAMEGRAPH_REVISION=41fee1f99f9276008b7cd112fca19dc3ea84ac32
RUN git init /opt/FlameGraph \
    && git -C /opt/FlameGraph remote add origin https://github.com/brendangregg/FlameGraph.git \
    && git -C /opt/FlameGraph fetch --depth 1 origin "${FLAMEGRAPH_REVISION}" \
    && git -C /opt/FlameGraph checkout --detach FETCH_HEAD
ENV FLAMEGRAPH_DIR=/opt/FlameGraph

# Prepare a private virtual DPDK link before running the benchmark as the host
# user. The project is bind-mounted at --workdir at runtime.
COPY --chmod=0755 scripts/bench-dpdk-entrypoint.sh /usr/local/bin/bench-dpdk-entrypoint
ENV PYTHONDONTWRITEBYTECODE=1
ENTRYPOINT ["/usr/local/bin/bench-dpdk-entrypoint"]
