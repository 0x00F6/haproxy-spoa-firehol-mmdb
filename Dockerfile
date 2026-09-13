# syntax=docker/dockerfile:1

# ---------------------------------------------------------------------------
# Builder: only the compiler, libc development files and build tools come
# from Ubuntu. Third-party libraries are built by cmake/dependencies.
# ---------------------------------------------------------------------------
FROM ubuntu:26.04 AS base

FROM base AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG BUILD_FULL_STATIC=ON

# Core build toolchain
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt/lists,sharing=locked \
    --mount=type=cache,target=/root/.cache/pip \
        apt-get update \
        && apt-get install -y --no-install-recommends \
            build-essential gcc-15 g++-15 cmake meson ninja-build pkg-config diffutils make \
            python3 python3-pip python3-venv python3-yaml python3-pyelftools \
            curl ca-certificates git \
            ragel valgrind gettext autoconf automake libtool \
            perl m4 texinfo gawk bison flex \
            systemtap-sdt-dev \
        && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the whole project so cmake/Seastar.cmake and its templates are present
COPY CMakeLists.txt CMakeLists.txt
COPY Makefile Makefile
COPY cmake cmake
COPY src src
COPY tests tests
# .git is needed at configure time so SPOA_SERVER_VERSION resolves to the last git
# tag (or short commit hash) for argparse --version.
COPY .git .git

RUN --mount=type=cache,target=/app/.cache \
    make build BUILD_FULL_STATIC=${BUILD_FULL_STATIC} \
    && cp build/haproxy-spoa-firehol-mmdb /app/haproxy-spoa-firehol-mmdb

# Static builds have no ELF interpreter. For optional dynamic builds, collect
# the shared-library closure from the builder.
RUN set -eu; \
    mkdir -p /runtime-libs; \
    if ldd /app/haproxy-spoa-firehol-mmdb > /tmp/runtime-libs 2>&1; then \
        cat /tmp/runtime-libs; \
        if grep -q 'not found' /tmp/runtime-libs; then exit 1; fi; \
        awk '/=> \// { print $3 } /^[[:space:]]*\// { print $1 }' /tmp/runtime-libs \
            | xargs -r cp -L --parents -t /runtime-libs; \
    else \
        grep -q 'not a dynamic executable' /tmp/runtime-libs; \
    fi

# Keep glibc runtime support on the same base as the builder.
# GnuTLS links the source-built zlib archive directly.
FROM base AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /runtime-libs/ /
COPY --from=builder /app/haproxy-spoa-firehol-mmdb /app/haproxy-spoa-firehol-mmdb

# Check startup in the final filesystem.
RUN /app/haproxy-spoa-firehol-mmdb --help > /dev/null

ENTRYPOINT ["/app/haproxy-spoa-firehol-mmdb"]
