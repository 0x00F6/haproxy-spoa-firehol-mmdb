# SPOE performance benchmark

Run from the repository root with Docker and Make installed:

```sh
make bench
make bench BENCH_ARGS='--duration 30 --warmup 3 --connections 16'
```

`make bench` builds `bench.Dockerfile`, then compiles and benchmarks the
application inside that image. It includes GCC, CMake, source-dependency build
tools, Python and Linux perf; none of these are required on the host.

The benchmark launches `./build/haproxy-spoa-firehol-mmdb` with
`--seastar-conf config/seastar.conf --io-conf config/io.conf`. The supplied
configuration uses the Linux TCP/IP stack (`posix`) over loopback. Pass these
same path options through `BENCH_ARGS` to select other files; the Python script
also accepts them and defaults to these repository files.

For DPDK AF_PACKET, use
`make bench BENCH_ARGS='--duration 30 --warmup 5 --connections 300 --dpdk-interface spoa-dpdk'`.
The entrypoint prepares a virtual Ethernet pair inside the container: Linux owns
`198.18.0.1/30` on `spoa-client`, while `spoa-dpdk` has no Linux IPv4 address
and Seastar owns `198.18.0.2`. Packet offloads are disabled so Seastar receives
complete packets with valid checksums. This explicit option selects the native
stack and DPDK, uses ordinary memory and disables PCI probing. No host hugepage
reservation or NIC binding is needed. A configured physical DPDK NIC requires
`--network-stack=native --dpdk-pmd` when launching the application directly.

The repository is bind-mounted at the same absolute path to reuse downloaded
dependencies under `.cache/deps/`. The container builds the profiled application
in `build/` (override with `BUILD_DIR`). The host's `build/bench/` is mounted at `/results`
to preserve the SVG, perf data and `benchmark-*.log` console summaries after the
container exits. Files use your host UID/GID. Override `BENCH_OUTPUT_DIR` to
change the results directory, or `BENCH_IMAGE` to change the local image tag.
The default MMDB is the repository's `firehol.mmdb`; `MMDB_PATH` and
`DROP_BY_CATEGORY` environment variables are passed through. Paths must be
accessible through the repository mount.

The benchmark container receives `NET_ADMIN`, `PERFMON` and `SYS_PTRACE` capabilities and
runs without Docker's seccomp filter to allow perf attachment and stack sampling.
Its entrypoint prepares the private link as root, then drops to the host UID/GID
with `NET_RAW` and `NET_ADMIN` retained for AF_PACKET sockets and interface
initialization inside the private network namespace.
The image also assigns these file capabilities to `/usr/bin/perf`, so profiling
works when the container runs with your non-root UID/GID.
It does not use privileged mode, the host PID namespace, published ports or
change host sysctls. A perf preflight runs before compilation. Rootless Docker or
host security policies may still disallow profiling, in which case the command
fails with a diagnostic. The normal application container is unaffected.

To measure an existing host binary without Docker (Python 3.10+ required), use
the default POSIX network stack over loopback:

```sh
python3 tests/benchmark_spoa.py --binary build/haproxy-spoa-firehol-mmdb \
  --seastar-conf config/seastar.conf --io-conf config/io.conf
```

For DPDK AF_PACKET, first prepare an equivalent virtual link and grant the
process `NET_RAW` and `NET_ADMIN`, then add
`--dpdk-interface spoa-dpdk --host 198.18.0.2`.

## CPU flamegraph

Flamegraphs use the official [Brendan Gregg FlameGraph scripts](https://github.com/brendangregg/FlameGraph):
`stackcollapse-perf.pl` folds the perf stacks and `flamegraph.pl` generates the
interactive SVG (click to zoom, Search to highlight functions). Docker installs
the revision pinned by `FLAMEGRAPH_REVISION` in `bench.Dockerfile`. For direct
host execution, install Perl and set `FLAMEGRAPH_DIR` to a FlameGraph checkout.

`make bench` enables `SPOE_PROFILE=ON`, keeping application debug information and
symbols while retaining Release `-O3` and LTO. It records only the server process
and its threads using Linux `perf`, starting after warmup and stopping after the
measured requests finish. Python client processes are excluded. MMDB reload
threads are included. Profiling adds overhead to the benchmark.

The summary links to a new directory under `build/bench/` for each run:

- `flamegraph.svg`: standalone SVG; open in a browser and hover over a frame for
  its function name, sample count and percentage. Wider frames represent more
  inclusive CPU samples; the horizontal position is not a timeline.
- `flamegraph.top.txt`: top leaf functions by self CPU samples.
- `flamegraph.folded`: aggregated stacks for other flamegraph viewers.
- `flamegraph.functions.txt`: full function names and inclusive sample counts.
  The SVG is 2400 pixels wide; narrow labels remain truncated, with full names
  available on hover when the SVG is opened directly in a browser.
- `perf.data`, `perf-script.txt` and logs: raw profiling evidence.

This is statistical **user-space CPU profiling**, not elapsed time per function
or off-CPU waiting time. Optimized/inlined functions and libraries without debug
information may produce incomplete stacks or unknown symbols.
Samples without a decoded stack remain attributed to their process/thread root
by the upstream converter and are included in the percentages. They are not
resolved function self time. The converter input is normalized to one unit per
sample, rather than perf event-period weights. Raw perf files
can be large. The benchmark uses 99 Hz sampling and DWARF stack unwinding so
dependencies do not need to be rebuilt with frame pointers.

Linux perf is included in `bench.Dockerfile`. For direct Python profiling on the
host, install perf there and ensure the kernel allows it through `CAP_PERFMON`
or a suitable `perf_event_paranoid` policy. If perf is unavailable or denied,
the benchmark reports an error instead of generating an empty graph.

For a benchmark without profiling, run the Python command above directly.
To profile an existing binary built with symbols:

```sh
python3 tests/benchmark_spoa.py --flamegraph build/bench --duration 30 --connections 300
```

The load generator uses multiple Python processes, defaulting to one per available
CPU, capped by the total connection count. Connections are divided across those
processes; `--connections 300` still means 300 connections in total. Use
`--workers N` to tune the client process count (`--workers 1` reproduces the
single-process client). All workers finish their handshakes and warmup before
starting the measured phase at a common deadline.

The summary reports client and server CPU consumption separately, with 100%
meaning one logical CPU. Client usage sums measured CPU time across the load
processes. Server usage reads Linux `/proc` process CPU time across all threads
over the measured phase and result collection. Active shards indicate that work
was distributed, not that each CPU reached full utilization. A local Python
client shares the host's CPUs with Seastar and can still limit throughput even
with multiple processes.

The benchmark starts its own server on temporary ports at the selected address.
Memory, logging, reactor and shard settings come from the selected configuration
files and Seastar defaults. Use `--smp N` to override the shard count explicitly.
It stops that server on exit. Each shard owns a SPOE listener and request
processor; connections are distributed by Seastar. The summary discovers the
actual shard count from Prometheus metrics and shows which shards performed MMDB
lookups and each shard's lookup count during measurement. These counts exclude warmup. Use enough connections
to exercise all shards; a single connection stays on one shard.
During the measured phase, the coordinator updates the MMDB file timestamps
once per second (first touch after one second, no touches during warmup).
This modifies the selected file's timestamps, not its contents. The watcher
handles `IN_ATTRIB` events and publishes a fresh snapshot on each successful
reload. The summary distinguishes touches sent from actual successful reloads,
excluding the initial database load. Because each shard owns its database,
"DB rotations" is the minimum reload count across shards; the total and the
per-shard counts are also displayed. Inotify can coalesce events, so touch and
reload counts need not match. The benchmark waits up to `--timeout` seconds for
pending reloads after measurement; this wait is excluded from throughput.
It requires `firehol.mmdb`, or a database selected using `--mmdb PATH` or
`MMDB_PATH`. Categories default to `unroutable,abuse`; override them with
`--categories` or `DROP_BY_CATEGORY`.

Each TCP connection performs a SPOE handshake, then sends one `check-ip` request
at a time. Every response must contain the corresponding stream/frame IDs and a
valid `ip_bad` boolean ACK. Warmup requests are excluded from the final results.
The default workload generates a random IP for each request, choosing IPv4 or
IPv6 with equal probability and sampling uniformly across the full address range
(including reserved addresses). Repeated `--ip ADDRESS` options override this
with a fixed list. These addresses are sent to the local SPOE server for lookup;
the benchmark does not connect to them. The summary explicitly counts responses
with `ip_bad = 0` and `ip_bad = 1`, excluding warmup and errors. Their distribution
depends on the chosen database and categories.

The English summary reports successful requests, errors, blocked/clear decisions,
requests per second, SPOE payload traffic including framing, and latency in
milliseconds (min, mean, max, p50, p95, p99). Each client process retains a uniform
reservoir of at most 100,000 successful requests. Global percentiles weight each
sample by the number of requests it represents, so processes with different
throughputs contribute proportionally. Min, mean, max and
throughput use all successful requests. The measured duration includes completion
of requests in flight at the deadline; failed requests are counted separately.
Connection setup and warmup are excluded. `--timeout` bounds each response wait.
Protocol errors, timeouts, startup failures or an empty measured run return a
nonzero exit code.

This measures round-trip performance including the Python load generator, TCP
and scheduling on the same machine, plus DPDK AF_PACKET and Linux virtual
interfaces when that mode is explicitly selected.
It does not measure a physical DPDK NIC or establish the server's maximum
capacity. Compare runs with the same database, IP workload, concurrency and host
load. There is no timing-based pass/fail threshold in CTest.

The benchmark reads `/metrics?__aggregate__=false` for per-shard diagnostics.
The standard `/metrics` endpoint aggregates application counters across shards
and omits their `shard` label.
