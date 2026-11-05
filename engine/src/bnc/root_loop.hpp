#pragma once

#include <cstdint>
#include <vector>

#include "app/telemetry.hpp"
#include "bnc/pricing.hpp"
#include "core/instance.hpp"
#include "cuts/blossom.hpp"
#include "cuts/mincut.hpp"
#include "cuts/pool.hpp"
#include "graph/candidate_graph.hpp"
#include "lp/dual_simplex.hpp"

namespace apex {

struct RootLoopConfig {
    SimplexOptions lp;
    SecSeparatorConfig sec;
    BlossomConfig blossom;
    bool use_blossom = true;
    uint32_t age_limit = 10;
    uint32_t max_cuts_per_round = 500;  // most violated first
    uint32_t max_rounds = 10000;
    double time_limit_s = INFINITY;
    int64_t ub = -1;                    // integer tour cost for root RCF (-1: skip RCF)
    bool compute_global_bound = true;   // Farley bound over the complete graph (O(n^2))
    // Tailing-off stop: quit when the LP value gained less than tailing_rel * |z| over the last
    // tailing_window rounds (reported as tailing_off, never as converged).
    uint32_t tailing_window = 50;
    double tailing_rel = 1e-4;
    // Pricing (exact reduced costs over the complete graph) every round: edges with negative
    // reduced cost are added as columns (up to pricing_batch, most negative first). Convergence
    // requires no violated cut AND no negative edge; then global_lb == lp_value.
    bool price = true;
    uint32_t pricing_batch = 2000;
    uint32_t max_pricing_rounds = 1000000;
    PricingMode pricing_mode = PricingMode::Grid;  // exact in every mode; Scan is the O(n^2) reference
};

struct RootRoundLog {
    uint32_t round;
    double lp_value;
    double global_lb;  // Farley bound at this round (valid for the complete graph)
    uint32_t rows, new_component, new_sec, new_blossom, reactivated, aged_out;
    uint64_t lp_iterations;
    double lp_s, sep_s;
};

struct RootLoopResult {
    SimplexResult::Status status;
    bool converged = false;          // no family found a violated cut
    bool tailing_off = false;        // stopped by the tailing-off rule (violated cuts may remain)
    bool infeasible = false;         // proven infeasible over the complete graph (Farkas pricing)
    bool cutoff = false;             // stopped: ceil(global_lb - 1e-6) >= NodeLimits::cutoff
    double lp_value = 0;             // z over the candidate graph (degree + cuts)
    double global_lb = -INFINITY;    // valid for the complete graph (Farley, exact reduced costs)
    int64_t global_lb_int = 0;       // ceil(global_lb - 1e-6)
    uint64_t negative_rc_edges = 0;  // non-candidate edges with negative reduced cost (final)
    uint32_t pricing_rounds = 0;
    uint32_t first_dual_flips = 0;
    uint64_t pricing_calls = 0;      // complete-graph pricing passes (each one a verification)   // initial dual-feasibility repairs of the first solve (warm-start check)
    uint64_t priced_edges = 0;       // columns added by pricing (status kNewPricing)
    uint32_t rcf_fixed0 = 0, rcf_fixed1 = 0;
    uint64_t cuts_sec = 0, cuts_blossom = 0, cuts_component = 0;
    double lp_s = 0, sep_s = 0, pricing_s = 0, total_s = 0;
    std::vector<RootRoundLog> rounds;
    PoolStats pool;
    Lp lp;                           // final root LP (run_root_loop only; bounds include RCF fixes)
    SimplexResult final;
};

// x_edge fixed to value (0 or 1) on top of the root bounds (edge status kFixed0 / kFixed1).
struct BoundChange {
    uint32_t edge;
    uint8_t value;
};
using BoundChanges = std::vector<BoundChange>;

// A warm-start basis together with the pool indices of its cut rows (row n + k <-> active[k]).
struct NodeBasis {
    Basis basis;
    std::vector<size_t> active;
};

// Per-call limits of the cut-and-price loop.
struct NodeLimits {
    uint32_t max_rounds = 10000;      // only while x is fractional
    uint32_t tailing_window = 50;     // 0 disables; only while x is fractional
    double tailing_rel = 1e-4;
    double time_limit_s = INFINITY;
    int64_t cutoff = -1;              // stop once ceil(global_lb - 1e-6) >= cutoff (-1: never)
};

// Cut-and-price loop over the candidate graph. The LP state (cut rows, priced columns, pool,
// basis) persists across calls, so tree nodes reuse everything found before. Each call:
// solve => pool re-check => components/SEC => blossom (when SEC finds nothing) => add violated
// rows => exact pricing over the complete graph (negative columns added) => warm re-solve,
// until no cut and no negative edge remains (converged), a limit stops it, the Farley bound
// reaches the cutoff, or the LP is proven infeasible (Farkas pricing finds no repairing edge).
// Non-binding cuts (basic logical, zero dual for age_limit re-solves) leave the LP.
class CutLoop {
public:
    CutLoop(const Instance& inst, CandidateGraph& g, const RootLoopConfig& cfg, TelemetrySink* tel = nullptr);
    // bc: node bounds on top of the edge-status bounds. warm: start basis (nullptr: current).
    RootLoopResult solve(const BoundChanges& bc, const NodeBasis* warm, const NodeLimits& lim);
    NodeBasis basis() const { return {basis_, active_}; }
    const Lp& lp() const { return lp_; }
    const CutPool& pool() const { return pool_; }
    // Columns present when `b` was taken equal the current ones (no pricing since).
    bool same_columns(const NodeBasis& b) const { return b.basis.col.size() == g_.edges.size(); }

private:
    void build_lp();
    void apply_bounds();
    void adopt(const NodeBasis& warm);
    size_t price(const SimplexResult& res, double& glb, RootLoopResult& out, bool farkas);

public:
    const PricingStats& pricing_stats() const { return pricer_.stats(); }

private:

    const Instance& inst_;
    CandidateGraph& g_;
    RootLoopConfig cfg_;
    TelemetrySink* tel_;
    CutPool pool_;
    EdgePricer pricer_;
    std::vector<size_t> active_;
    Lp lp_;
    Basis basis_;
    bool warm_ = false;
    BoundChanges bc_;
};

// Root call of CutLoop (no branching) followed by global Farley bound and root RCF.
RootLoopResult run_root_loop(const Instance& inst, CandidateGraph& g, const RootLoopConfig& cfg,
                             TelemetrySink* tel = nullptr);

}  // namespace apex
