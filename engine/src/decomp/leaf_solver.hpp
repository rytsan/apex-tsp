#pragma once

// heuristic: leaf tours for the scalable mode. UB only: no leaf bound is ever reported or used.

#include <cstdint>
#include <vector>

#include "core/instance.hpp"
#include "decomp/kdtree.hpp"

namespace apex {

struct LeafConfig {
    uint32_t exact_limit = 300;          // --leaf-exact-limit: leaves this small go through mini B&C
    uint64_t exact_max_nodes = 500;      // node cap of a leaf B&C (deterministic, no time limit)
    uint64_t ils_iterations_per_node = 10;  // leaf ILS budget = this * leaf size
    uint64_t seed = 1;
    uint32_t threads = 1;                // leaves are independent: same result for any count
};

struct LeafStats {
    uint32_t leaves = 0, exact_leaves = 0, exact_proven = 0;
    uint32_t min_size = 0, max_size = 0;
};

// Sub-instance on `ids` (coordinates, metric and original ids copied).
Instance sub_instance(const Instance& inst, const std::vector<uint32_t>& ids);

// heuristic: one Hamiltonian cycle (global ids) per leaf of `tree`, indexed like `tree` (empty
// for internal nodes). Leaf solve: Hilbert start + 2-opt + ILS (seed derived from the leaf
// index); leaves with <= exact_limit points then run a mini B&C from that tour and keep its UB.
std::vector<std::vector<uint32_t>> solve_leaves(const Instance& inst, const std::vector<KdNode>& tree,
                                                const LeafConfig& cfg, LeafStats* stats = nullptr);

}  // namespace apex
