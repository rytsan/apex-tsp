#pragma once

#include <cstdint>
#include <string>

#include "core/instance.hpp"
#include "graph/tours.hpp"

namespace apex {

// FNV-1a 64 over n, metric and the bit patterns of all coordinates.
uint64_t instance_hash(const Instance& inst);

struct CheckpointV1 {
    uint64_t instance_hash = 0;
    std::string instance_name;
    std::string mode;
    uint64_t seed = 0;
    uint32_t knn = 8;
    double time_limit_s = 0;
    std::string rng_state;   // std::mt19937_64 textual state (standardised format)
    double wall_s = 0;       // accumulated wall clock across runs
    Tour best;
};

// File name inside the --checkpoint directory.
inline constexpr const char* kCheckpointFile = "checkpoint.v1";

// Atomic: writes <dir>/checkpoint.v1.tmp, flushes, then renames over the target.
void write_checkpoint(const std::string& dir, const CheckpointV1& c);

// Validates header, tour permutation and that best.cost matches the recomputed
// cost against inst; throws std::runtime_error on mismatch (incl. instance hash).
CheckpointV1 read_checkpoint(const std::string& dir, const Instance& inst);

}  // namespace apex
