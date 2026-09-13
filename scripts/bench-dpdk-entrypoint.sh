#!/bin/bash
set -euo pipefail

: "${SPOA_BENCH_UID:?Pass the host user ID in SPOA_BENCH_UID}"
: "${SPOA_BENCH_GID:?Pass the host group ID in SPOA_BENCH_GID}"

# Prepare an optional link for BENCH_ARGS='... --dpdk-interface spoa-dpdk'.
# Docker's private network namespace contains both ends of the benchmark link.
# Linux serves the client address; Seastar owns the DPDK end and its IP address.
ip link add spoa-client type veth peer name spoa-dpdk
ip address add 198.18.0.1/30 dev spoa-client
for interface in spoa-client spoa-dpdk; do
    ip link set "$interface" up
    # AF_PACKET exchanges complete packets with Seastar's userspace stack.
    # Disable each supported offload, including software segmentation on veth.
    for feature in rx tx tso gso gro; do
        ethtool -K "$interface" "$feature" off 2>/dev/null || true
    done
done

# AF_PACKET needs packet sockets and interface-flag/MAC restoration at startup.
# Keep those capabilities in Docker's private network namespace while artifacts
# use the host user's ownership. perf has its separate file capabilities.
exec setpriv --reuid "$SPOA_BENCH_UID" --regid "$SPOA_BENCH_GID" \
    --clear-groups --inh-caps +net_raw,+net_admin --ambient-caps +net_raw,+net_admin \
    /usr/bin/make bench-container "$@"
