#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bnc/branching.hpp"
#include "bnc/primal_heur.hpp"
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
    // Branching that created this node (pseudocost update once it is solved).
    int64_t branch_edge = -1;
    uint8_t branch_dir = 0;
    double branch_frac = 0, parent_lp = 0;
};

class BackgroundIls;

struct BnCConfig {
    RootLoopConfig root;                           // cut loop (also used at nodes)
    enum class Select { DiveThenBest, BestBound } select = Select::DiveThenBest;  // --node-select
    NodeLimits node;                               // per-node cut loop limits (cutoff set by the tree)
    BranchingConfig branching;
    bool lp_heuristic = true;                      // LP-guided tour at every fractional node (UB only)
    // Background ILS (M4.1, UB only, not owned): polled at node boundaries. UB arrival depends on
    // thread timing, so runs with it are not bitwise reproducible (proofs stay valid).
    const BackgroundIls* primal = nullptr;
    uint32_t dive_depth_limit = 50;
    // M4.2 heuristics (UB only): LP diving at fractional nodes, RIH-lite once at the root
    // (rih_iterations ILS iterations; 0 = off).
    DiveConfig dive;
    uint64_t rih_iterations = 2000;                // dive -> best-bound once a node this deep is processed
    std::string tour_path;                         // .tour rewritten atomically on every UB improvement
    double time_limit_s = INFINITY;                // --time-limit (anytime: in-flight node finishes)
    uint64_t max_nodes = UINT64_MAX;
    // Checkpoint v2 (M3.6): <dir>/LATEST names the current v2-<seq>/ snapshot (manifest + files).
    std::string checkpoint_dir;                    // --checkpoint
    double checkpoint_every_s = 300;               // also written whenever the run stops early
    bool resume = false;                           // --resume: continue from checkpoint_dir (initial tour ignored)
    uint64_t seed = 1;                             // recorded in the manifest
    // Parallel tree (M3.6): nodes processed in deterministic batches of parallel_lps workers, each on
    // a snapshot of the shared state, merged in worker order. 1 = serial. Capped at 2 in Sprint 3.
    uint32_t parallel_lps = 1;                     // --parallel-lps
    uint64_t ram_cap_bytes = uint64_t(2) << 30;    // peak RSS asserted below this with parallel_lps > 1
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
    uint64_t warm_dual_infeasible = 0;
    // Certificate: every pruning used a Farley bound from exact pricing over the complete graph,
    // so verification pricing (MASTER §5.6) runs at every node, not only at the end.
    uint64_t verification_pricing_calls = 0, priced_columns = 0;
    uint64_t rcf_node_fixings = 0;     // node-scoped fixes passed to children
    uint64_t strong_pivots = 0, strong_candidates = 0, max_node_strong_pivots = 0;  // warm starts whose basis needed dual repair with unchanged columns (must stay 0)
    uint64_t heuristic_calls = 0;
    uint64_t dive_calls = 0, dive_tours = 0, dive_pivots = 0;
    bool rih_improved = false;
    double heuristic_s = 0;
    double root_lb = -INFINITY, lp_s = 0, sep_s = 0, pricing_s = 0, total_s = 0;
    RootLoopResult root;
    std::vector<uint32_t> processing_order;  // node ids in processing order (determinism tests)
    uint64_t checkpoints_written = 0, batches = 0;
    double elapsed_s = 0;            // wall clock accumulated across resumed runs
};

// Integral LP solution that is a single Hamiltonian cycle on the LP columns => tour; else empty.
Tour extract_tour(const Instance& inst, const CandidateGraph& g, const std::vector<double>& x);

// Global branch-and-cut (exact mode). Root = CutLoop + root RCF with the initial tour as UB; every
// node re-runs the cut-and-price loop under its bound changes from the parent basis. Node bounds
// are Farley bounds over the complete graph, so pruning is globally valid. Branching:
// reliability branching (strong branching until pseudocosts are reliable, M3.2).
BnCResult run_bnc(const Instance& inst, CandidateGraph& g, const BnCConfig& cfg, Tour initial,
                  TelemetrySink* tel = nullptr);

const char* status_name(BnCResult::Status s);

}  // namespace apex
