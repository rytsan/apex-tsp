#pragma once

// heuristic: patching of leaf tours into one tour (scalable mode). Produces a UB candidate and
// statistics ONLY; no local bound exists in this layer, none may ever feed pruning (MASTER §2,
// directive 5). Telemetry phase: "decomposition (heuristic)".

#include <cstdint>
#include <vector>

#include "core/instance.hpp"
#include "decomp/kdtree.hpp"
#include "decomp/leaf_solver.hpp"
#include "graph/candidate_graph.hpp"
#include "graph/tours.hpp"

namespace apex {

struct PatchConfig {
    uint32_t portals = 4;           // nearest cross-boundary pairs considered per merge
    uint32_t boundary_sample = 64;  // points per side closest to the split plane
};

struct DecompConfig {
    uint32_t max_leaf = 800;        // --leaf-size
    LeafConfig leaf;
    PatchConfig patch;
};

struct DecompStats {
    LeafStats leaf;
    uint32_t merges = 0;
    int64_t leaf_sum = 0, patched_cost = 0, final_cost = 0;
    uint64_t polish_moves = 0;
    double leaf_s = 0, patch_s = 0;
};

// heuristic: merges cycles a and b (disjoint, global ids) of the two sides of a split. Portals:
// the `portals` closest pairs among the `boundary_sample` points of each side nearest the split
// plane. Stitch: the cheapest exchange over every tour edge at a portal endpoint on each side
// ({a1,a2}, {b1,b2} -> {a1,b1}, {a2,b2} or {a1,b2}, {a2,b1}); exhaustive, hence optimal over
// the portal edges. Appends the four stitch endpoints to `touched`.
std::vector<uint32_t> merge_cycles(const Instance& inst, const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
                                   uint8_t axis, double split, const PatchConfig& cfg, std::vector<uint32_t>& touched);

// heuristic: KD-tree leaves solved independently, merged bottom-up, then LK-lite + Or-opt
// cleanup from every stitch endpoint on the global candidate graph g.
Tour decompose_and_patch(const Instance& inst, const CandidateGraph& g, const DecompConfig& cfg,
                         DecompStats* stats = nullptr);

}  // namespace apex
