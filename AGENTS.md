# AGENTS.md

Guidelines for AI agents and contributors working on this repository.

## Project overview

`haproxy-spoa-firehol-mmdb` is a C++20 SPOA (Stream Processing Offload Agent)
for HAProxy. It looks up client IPs in a MaxMind DB (MMDB) reputation database
generated from FireHOL blocklist ipsets, and answers HAProxy with a boolean
`ip_bad` decision. It is built on Seastar and runs one worker (shard/thread)
per CPU core.

The agent is horizontally constrained and performance-critical: the default
build is a fully static binary with `-O3`, whole-program LTO and stripped
symbols (`-flto -flto-partition=none`, `-s`). The request path is designed to
be zero-copy (views into Seastar buffers and into the mapped database, no
per-request heap allocation).

Everything that blocks (Git fetch/clone, blocklist compilation, copying and
unmapping the database) runs on helper threads, never on a Seastar reactor.

## Toolchain and build

- C++20 (`C++20` REQUIRED), compiled with GCC 15 (`gcc-15` / `g++-15`), set in
  `CMakeLists.txt` and the Makefile.
- Coroutines enabled via `-fcoroutines` (set in `CMakeLists.txt` and the
  Makefile `CXXFLAGS`). The codebase uses Seastar `future<>` continuations
  rather than keyword-based coroutines.
- CMake >= 3.24 + Ninja; the `Makefile` is the primary entry point.
- All third-party dependencies (Seastar, DPDK, Boost, OpenSSL, GnuTLS,
  libmaxminddb, libgit2, fmt, argparse, ...) are built **from source** as
  static `-O3` archives by `cmake/*.cmake` scripts, from SHA-256-verified
  archives cached in `.cache/deps/`. The first build is very long; subsequent
  builds reuse the cache.
- A separate static library `spoa_core` (`src/app_config.cpp`, SPOE protocol,
  mmdb, drop categories, builder, job scheduler) stays independent of the
  Seastar runtime; the final executable also links `spoa_server`,
  `spoa_agent`, `mmdb_reload`, the FireHOL compiler/git and the metrics
  modules.

Typical commands:

```sh
make build                # configure + build the agent (build/haproxy-spoa-firehol-mmdb)
make test                 # build then run all CTest tests (needs firehol.mmdb)
make run                  # build and run the agent
make fmt                  # clang-format -i all project sources (sort-includes)
make check                # clang-format --dry-run -Werror (must pass before commit)
make bench                # Docker CPU/flamegraph benchmark
make clean                # removes build/, cmake-build-*, .cache/, firehol-blocklist-ipsets/
```

Additional/build options:

```sh
make build SPOE_DEPS_JOBS=2 SPOA_DEPS_PARALLEL=4
make build BUILD_FULL_STATIC=OFF     # dynamic native runtime instead of -static
make bench BENCH_ARGS='--duration 30 --warmup 3 --connections 64 --smp 4 --workers 4'
make bench-dpdk BENCH_FLAMEGRAPH=1   # Seastar native stack via DPDK AF_PACKET
```

`SPOA_DEPS_PARALLEL` is the number of libraries built at once (derived from
CPU count and RAM by default), `SPOE_DEPS_JOBS` the jobs per library.
`SPOE_PROFILE` keeps symbols for profiling (used by the bench targets).

`make test` runs `ctest --test-dir build -j$(nproc) --output-on-failure`.
Some tests are conditional: `mmdb_reload_test` and extra `--mmdb` arguments of
`spoa_agent_test` only run when `firehol.mmdb` is present at the source root.

The binary accepts Seastar options (`--help-seastar`) and application options
(`--help`): `--seastar-conf`, `--io-conf`, `--dpdk-interface`, `--version`,
`--fetch-and-create-mmdb`, `--smp`, `--reactor-backend`, `--network-stack`,
`--dpdk-pmd`.

## Configuration

The agent is configured through environment variables (see `src/app_config.*`
and the README):

| Variable | Purpose |
| --- | --- |
| `MMDB_PATH` | Path of the generated/opened MMDB; empty disables generation and lookups |
| `FIREHOL_GIT_PATH` | FireHOL blocklist Git checkout directory |
| `FIREHOL_GIT_REPO_URL` | Repo cloned/fetched before MMDB generation |
| `DROP_BY_CATEGORY` | Comma-separated categories that flag an IP as bad (exact, case-sensitive) |
| `SERVER_LISTEN_ADDRESS` | SPOE listen address (default `0.0.0.0:9000`) |
| `METRICS_LISTEN_ADDRESS` | Prometheus metrics address (default `0.0.0.0:9100`) |
| `SSL_CERT_FILE` / `SSL_CERT_DIR` | CA certificates used to verify HTTPS Git remotes |

Behavioral notes to preserve:
- At startup, if `MMDB_PATH` is set, the agent first synchronizes the FireHOL
  checkout with the remote `master` branch and compiles the local blocklists
  into the database (temp file + atomic rename). A scheduler thread then
  repeats this at the beginning of each hour (`0 * * * *`).
- `--fetch-and-create-mmdb` runs that job once and exits (no scheduler).
- Whole-file generation uses "copy to a private inode, then swap the
  snapshot" so reloads (via inotify) never expose a truncated database to
  readers.

## Code layout

Sources are grouped by domain under `src/`; each directory is a
self-contained module with its own headers.

| Path | Responsibility |
| --- | --- |
| `src/main.cpp` | Seastar `app_template` setup, CLI options, startup/shutdown orchestration |
| `src/app_config.*` | Environment configuration into `AppConfig` |
| `src/spoa/spoa_server.*` | Per-shard listener, connections, frame transport (pipelining via `seastar::circular_buffer`) |
| `src/spoa/spoa_agent.*` | Frame handling, MMDB lookups, `AGENT-ACK` responses |
| `src/spoa/frame.*`, `message.h`, `typed_data.*` | SPOE protocol types, parsing, encoding |
| `src/spoa/protocol_io.h`, `varint.h` | Bounds-checked reader, wire encoding |
| `src/spoa/drop_categories.*` | Selected categories and per-category counters |
| `src/mmdb/mmdb.*` | `Mmdb`: MMDB ownership, lookups, categories, formatting |
| `src/mmdb/mmdb_reload.*` | `ReloadableMmdb`: inotify watch + atomic `shared_ptr` snapshot publication |
| `src/mmdb_builder/` | MMDB writer: trie, record pool, columnar deep merge |
| `src/firehol_blocklist_ipsets/` | libgit2 repository sync; parallel blocklist compilation |
| `src/job_scheduler/` | Cron-driven single-thread job runner (`croncpp.h` is vendored, do not reformat) |
| `src/metrics/` | Prometheus metric registries (spoa / firehol / scheduler) |
| `src/utils/` | Shared helpers: environment, strings, IP parsing, `UniqueFd`, off-reactor tasks (`blocking_task.h`) |
| `tests/` | CTest tests + benchmarks (`benchmark_spoa`, `benchmark_builder`) + flamegraph utilities |
| `cmake/` | Third-party dependency builds (from source) |
| `config/` | Seastar `seastar.conf` / `io.conf` templates |
| `haproxy/` | HAProxy `.cfg` files (SPOE rules + decision cache) |

## Code style

- **Formatting:** clang-format, 2-space indentation, braces on their own line
  (Allman). Includes are `--sort-includes`. Run `make fmt` / `make check`.
  The vendored `croncpp.h` keeps upstream formatting and is excluded.
- **Headers:** `#pragma once`; include guards are not used.
- **Namespaces:** `spoe` root, with sub-namespaces such as `spoe::utils`,
  `spoe::metrics`, `firehol_ipsets`, `job_scheduler`, `mmdb_builder`.
- **Naming:** `snake_case` for functions, methods, variables and fields;
  `kCamelCase` prefix for constants (e.g. `kStatusNotImplemented`,
  `kWhitespace`); `PascalCase` for types; `.h`/`.hpp` and `.cpp` extensions.
- **Doc comments:** `//! ...` one-line doxygen style for public/member APIs.
- **Annotations:** use `[[nodiscard]]` on allocation- or correctness-sensitive
  return values, and `noexcept` where the function cannot throw.
- **Includes:** project headers first (relative to `src/`, e.g.
  `"mmdb/mmdb.h"`), then system/standard-library headers, then third-party.
  clang-format requires sorted includes.
- **Comments:** explain *why*, not *what*; the codebase already comments
  invariants, lifetime rules and performance rationale — keep that style.
  Do not add code comments as filler.
- Use C++20 features (`std::format`, `std::to_chars`, `std::jthread`,
  `std::span`, `std::string_view`, structured bindings) over older
  alternatives. `std::format` is used in logs and error messages.
- `add_compile_options(-Wall -Wextra)` is global: project code must build
  warning-free.

## Concurrency and performance rules

- **Never block a Seastar reactor thread.** Git operations, blocklist
  compilation, MMDB copy/open/close (mmap/munmap) and snapshot teardown run
  on helper threads via `spoe::run_blocking` (`src/utils/blocking_task.h`).
  Blocking on a reactor in new code is a bug.
- Shards are isolated: each shard owns its listener, processor and a private
  database snapshot (`ReloadableMmdb`), which is what makes lookups lock-free.
- Snapshots are published through `std::atomic<std::shared_ptr<const Mmdb>>`;
  readers keep their snapshot until they finish consuming entry data and
  `string_view`s. Never retain a category view past the snapshot it borrows.
- Memory mapping and large-file copies are covered by inotify-driven
  reloads; keep the "copy to a private inode then swap the snapshot" pattern.
- Hot paths must remain zero/allocation-light: parse with `std::string_view`
  over the receive buffer, read categories directly from mapped memory, encode
  responses without per-request heap allocations.
- The scheduler thread runs one job at a time (cron `0 * * * *`); jobs must
  not overlap, and shutdown stops the scheduler and waits for an in-flight
  update. Metrics counters for firehol/scheduler are shared across threads and
  read on shard 0 — be mindful of atomicity.

## Testing

- Tests are standalone C++ executables registered as CTest targets in
  `CMakeLists.txt`, in addition to `tests` sources (each test is built from a
  `tests/test_*.cpp` file linked against the `spoa_core` static library, or
  against the full Seastar stack when they exercise the server/reload path).
- Test scope: SPOE parsing, drop categories, MMDB lookups/formatting, MMDB
  reloads, the agent over TCP, MMDB builder determinism, job scheduler,
  FireHOL compiler/jobs, utils, flamegraph and startup (`--help` via
  `tests/spoa_startup_test.cmake`).
- When adding a test: create `tests/test_*.cpp`, register a target and an
  `add_test` in `CMakeLists.txt` (mirror existing patterns, including
  conditional registration on `firehol.mmdb` presence), and use the matching
  link libraries (`spoa_core` and/or the Seastar `--start-group` block, e.g.
  with `src/mmdb/mmdb_reload.cpp` added through `target_sources`).
- Run `make test` and, before committing, `make check`.
- Benchmarks live in `tests/benchmark_spoa.cpp` and `tests/benchmark_builder.cpp`
  and are not CTest targets; `make bench` orchestrates the full run inside a
  Docker container (results under `build/bench/`, flamegraph + perf data).

## What NOT to do

- Do not reformat, "fix" or commit changes to vendored code: `croncpp.h`,
  `cmake/dependencies/`, and anything under `.cache/` or the dependency build
  trees.
- Do not commit build products (`build*/`, `cmake-build-*`), `.cache/`, the
  `firehol-blocklist-ipsets` checkout, `firehol.mmdb`, `data/`, or
  `config/seastar.conf.bench.bak` — they are gitignored.
- Do not commit standalone scratch scripts at the repo root (e.g. one-off
  `dummy.cpp`, `fix*.py`) unless explicitly asked.
- Do not add new third-party dependencies casually; they are built from source
  with pinned SHA-256 archives and add substantial build time. Prefer
  lightweight local implementations (see `utils/net_utils.h` which parses IPv4
  without libc).
- Do not weaken the static/optimized build defaults for regular changes; the
  `Release` profile (`-O3`, whole-program LTO, stripped) is the deployment
  target.
