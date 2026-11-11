#pragma once

// heuristic: patching of leaf tours into one tour (scalable mode). Produces a UB candidate and
// statistics ONLY; no local bound exists in this layer, none may ever feed pruning (MASTER §2,
// directive 5). Telemetry phase: "decomposition (heuristic)".

#include <cstdint>
#include <functional>
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
    bool density = true;            // density-aware cuts (M4.7); false: median KD split (M4.3)
    LeafConfig leaf;                // leaf.threads also runs the merges of one level in parallel
    PatchConfig patch;
    // Re-evaluation after each merge (M4.7): ILS on the merged cluster, kicks at the stitch and
    // boundary nodes, iterations = this * number of focus nodes (0 = off).
    uint32_t merge_iterations_per_focus = 5;
    // k-way merging (default): rounds over a minimum spanning tree (Prim) of the clusters, groups
    // of 2 (dense) up to max_group (sparse) clusters merged per round in parallel. false: the
    // binary bottom-up merge of the density tree (M4.7).
    bool kway = true;
    uint32_t max_group = 4;
    // Current cycles (every finished leaf / merged group so far), for the GUI; called under a
    // lock from worker threads, observation only.
    std::function<void(const std::vector<std::vector<uint32_t>>&)> on_forest;
};

struct DecompStats {
    LeafStats leaf;
    uint32_t merges = 0, levels = 0;
    int64_t leaf_sum = 0, patched_cost = 0, final_cost = 0;
    int64_t stitch_cost = 0, reeval_gain = 0;  // sum of stitch deltas / re-evaluation improvements
    uint64_t polish_moves = 0;
    double leaf_s = 0, patch_s = 0;
};

// heuristic: merges cycles a and b (disjoint, global ids) of the two sides of a split. Portals:
// the `portals` closest pairs among the `boundary_sample` points of each side nearest the split
// plane. Stitch: the cheapest exchange over every tour edge at a portal endpoint on each side
// ({a1,a2}, {b1,b2} -> {a1,b1}, {a2,b2} or {a1,b2}, {a2,b1}); exhaustive, hence optimal over
// the portal edges. Appends the four stitch endpoints to `touched`.
// focus (optional): receives the boundary samples of both sides plus the stitch endpoints.
std::vector<uint32_t> merge_cycles(const Instance& inst, const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
                                   uint8_t axis, double split, const PatchConfig& cfg, std::vector<uint32_t>& touched,
                                   std::vector<uint32_t>* focus = nullptr);

// heuristic: ILS on one cluster cycle (global ids, rewritten in place) on its own candidate
// graph, kicks next to `focus`. Returns the cost improvement.
int64_t reevaluate_cycle(const Instance& inst, std::vector<uint32_t>& cycle, const std::vector<uint32_t>& focus,
                         uint64_t iterations, uint64_t seed);

// heuristic: density tree (or KD) leaves solved in parallel, merged bottom-up level by level
// (merges of one level in parallel), each merge re-evaluated by focused ILS, then LK-lite +
// Or-opt cleanup from every stitch endpoint on the global candidate graph g. Deterministic for
// any thread count.
Tour decompose_and_patch(const Instance& inst, const CandidateGraph& g, const DecompConfig& cfg,
                         DecompStats* stats = nullptr);
// heuristic: density-tree leaves solved in parallel, then merge rounds until one cycle: Prim's
// minimum spanning tree over the clusters (weight: cheapest candidate edge between two clusters)
// picks the neighbours; groups grow along its edges, cheapest first, up to 2 clusters where the
// points are dense and up to max_group where they are sparse (points per bounding-box area vs
// the median). A group is stitched one cluster at a time with the cheapest 2-edge exchange over
// the candidate edges between them, then re-evaluated by ILS around the stitches. Groups of a
// round run in parallel; deterministic for any thread count.
Tour cluster_merge(const Instance& inst, const CandidateGraph& g, const DecompConfig& cfg, DecompStats* stats = nullptr);

}  // namespace apex
