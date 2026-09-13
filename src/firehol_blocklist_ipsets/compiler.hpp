#pragma once
#include <filesystem>

namespace firehol_ipsets {

// Parses an already prepared local repository and atomically replaces the MMDB.
void compile_to_mmdb(const std::filesystem::path &target_mmdb_path,
                     const std::filesystem::path &repository_path,
                     uint64_t last_commit_unix);

} // namespace firehol_ipsets
