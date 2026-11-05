#pragma once

#include <cstdint>
#include <vector>

#include "app/telemetry.hpp"
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
    double lp_value = 0;             // z over the candidate graph (degree + cuts)
    double global_lb = -INFINITY;    // valid for the complete graph (Farley, exact reduced costs)
    int64_t global_lb_int = 0;       // ceil(global_lb - 1e-6)
    uint64_t negative_rc_edges = 0;  // non-candidate edges with negative reduced cost (final)
    uint32_t pricing_rounds = 0;
    uint64_t priced_edges = 0;       // columns added by pricing (status kNewPricing)
    uint32_t rcf_fixed0 = 0, rcf_fixed1 = 0;
    uint64_t cuts_sec = 0, cuts_blossom = 0, cuts_component = 0;
    double lp_s = 0, sep_s = 0, pricing_s = 0, total_s = 0;
    std::vector<RootRoundLog> rounds;
    PoolStats pool;
    Lp lp;                           // final root LP (bounds include RCF fixes)
    SimplexResult final;
};

// Cut-and-resolve at the root (no branching): solve => pool re-check => components/SEC =>
// blossom (when SEC finds nothing) => add violated rows => warm-start re-solve, until no family
// yields a violated cut. Non-binding cuts (basic logical, zero dual for age_limit re-solves)
// leave the LP. After convergence: global Farley bound and root reduced-cost fixing with the
// sound integral rule (fix x_e = 0 iff ceil(global_lb + d_e - 1e-6) >= ub; symmetric for 1).
// RCF fixes are root-scoped (permanent): recorded in g.edges[].status and the LP bounds.
RootLoopResult run_root_loop(const Instance& inst, CandidateGraph& g, const RootLoopConfig& cfg,
                             TelemetrySink* tel = nullptr);

}  // namespace apex
