#pragma once

// Primal heuristics inside B&C (M4.2). heuristic: both produce UB candidates only; nothing here
// changes a bound, a basis kept by the tree, or a pruning decision.

#include <cstdint>
#include <vector>

#include "core/instance.hpp"
#include "graph/candidate_graph.hpp"
#include "graph/tours.hpp"
#include "lp/dual_simplex.hpp"
#include "primal/lk_lite.hpp"

namespace apex {

struct DiveConfig {
    uint32_t max_rounds = 5;          // dive rounds per node (0 = off)
    uint64_t pivots_per_round = 50;   // dual simplex iteration cap per re-solve
};

struct DiveResult {
    Tour tour;                        // empty when nothing was found
    bool integral = false;            // tour is an integral dive LP solution (else greedy completion)
    uint32_t rounds = 0;
    uint64_t pivots = 0;
};

// heuristic: dive from a fractional node LP solution x (over the LP columns = g.edges). Each
// round fixes every x_e > 1 - 1e-6 plus the largest fractional x_e (lowest index on ties) to 1
// and re-solves warm from the previous basis. The first infeasible re-solve backtracks once
// (that x_e is fixed to 0 instead); it stops at a second infeasible or a non-optimal re-solve
// (iteration cap), or at an integral solution (returned as a tour if it is one Hamiltonian cycle).
// Otherwise the deepest feasible dive solution is completed by lp_guided_tour. Bounds of `lp`
// are restored before returning.
DiveResult dive(const Instance& inst, const CandidateGraph& g, Lp& lp, const Basis& basis, const std::vector<double>& x,
                const SimplexOptions& opt, const DiveConfig& cfg);

// heuristic: RIH-lite. Fragments from the x_e = 1 edges, reconnected greedily over the candidate
// graph (then 2-opt / Or-opt), improved by ILS. Returns the better of that tour and `incumbent`.
Tour rih_lite(const Instance& inst, const CandidateGraph& g, const std::vector<double>& x, const Tour& incumbent,
              const IlsConfig& ils);

}  // namespace apex
