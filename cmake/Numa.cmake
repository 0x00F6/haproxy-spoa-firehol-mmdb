# numa 2.0.19: built from source as static archives with -O3.
include_guard(GLOBAL)
spoe_autotools(numa
    VERSION 2.0.19
    URL "https://github.com/numactl/numactl/releases/download/v2.0.19/numactl-2.0.19.tar.gz"
    SHA256 f2672a0381cb59196e9c246bf8bcc43d5568bc457700a697f1a1df762b9af884
    ARCHIVES numa
)
