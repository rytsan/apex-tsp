#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "bnc/root_loop.hpp"
#include "graph/tours.hpp"

namespace apex {

enum class NodeState : uint8_t { Open, Processing, Pruned, Infeasible, Integral, Branched };

struct BnCNode {
    uint32_t id, depth;
    double dual_bound;                             // parent's globally valid (Farley) bound
    BoundChanges bounds;                           // full list from the root
    std::shared_ptr<const NodeBasis> parent_basis; // shared by both children
    NodeState state;
};

struct BnCConfig {
    RootLoopConfig root;                           // cut loop (also used at nodes)
    enum class Select { DiveThenBest, BestBound } select = Select::DiveThenBest;  // --node-select
    NodeLimits node;                               // per-node cut loop limits (cutoff set by the tree)
    double time_limit_s = INFINITY;                // --time-limit (anytime: in-flight node finishes)
    uint64_t max_nodes = UINT64_MAX;
    BnCConfig() {
        node.max_rounds = 100;
        node.tailing_window = 10;
        node.tailing_rel = 1e-4;
    }
};

struct BnCResult {
    enum class Status { Optimal, TimeLimit, NodeLimit } status = Status::Optimal;
    Tour best;                       // best tour found (cost recomputed with the metric)
    int64_t ub = 0;
    double lb = -INFINITY;           // global: min over open nodes (== ub at proof)
    int64_t lb_int = 0;              // ceil(lb - 1e-6)
    uint64_t nodes = 0, pruned = 0, infeasible = 0, integral = 0, branched = 0, max_depth = 0;
    uint64_t ub_improvements = 0, warm_starts = 0, cold_starts = 0;
    uint64_t warm_dual_infeasible = 0;  // warm starts whose basis needed dual repair with unchanged columns (must stay 0)
    double root_lb = -INFINITY, lp_s = 0, sep_s = 0, pricing_s = 0, total_s = 0;
    RootLoopResult root;
    std::vector<uint32_t> processing_order;  // node ids in processing order (determinism tests)
};

// Integral LP solution that is a single Hamiltonian cycle on the LP columns => tour; else empty.
Tour extract_tour(const Instance& inst, const CandidateGraph& g, const std::vector<double>& x);

// Global branch-and-cut (exact mode). Root = CutLoop + root RCF with the initial tour as UB; every
// node re-runs the cut-and-price loop under its bound changes from the parent basis. Node bounds
// are Farley bounds over the complete graph, so pruning is globally valid. Branching here is
// most-fractional (M3.2 replaces it with reliability branching).
BnCResult run_bnc(const Instance& inst, CandidateGraph& g, const BnCConfig& cfg, Tour initial,
                  TelemetrySink* tel = nullptr);

const char* status_name(BnCResult::Status s);

}  // namespace apex
