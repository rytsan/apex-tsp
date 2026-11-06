#pragma once

#include <cstdint>
#include <vector>

#include "core/instance.hpp"
#include "graph/candidate_graph.hpp"

namespace apex {

// Held-Karp Lagrangian bound (M4.4): 1-tree relaxation with node multipliers pi, modified costs
// c_uv + pi_u + pi_v, L(pi) = w(min 1-tree) - 2 sum(pi). Special node: 0.
struct HkConfig {
    uint32_t max_iterations = 2000;  // subgradient iterations (cap)
    double time_limit_s = 0;         // ascent budget, 0 = none
    double lambda0 = 2;              // Polyak factor: t = lambda (ub - L) / ||g||^2
    uint32_t patience = 30;          // iterations without a new best before lambda halves
    double lambda_min = 1e-6;
    bool exact = true;               // final bound over the complete graph (dense Prim, O(n^2))
};

struct HkResult {
    double lb_candidate = 0;  // best L(pi) with 1-trees on the candidate graph: ascent only, NOT a bound
    double lb = 0;            // valid bound: complete-graph 1-tree at the best pi (exact = true)
    int64_t lb_int = 0;       // ceil(lb - 1e-6)
    bool valid = false;       // lb computed over the complete graph
    std::vector<double> pi;   // best multipliers
    uint32_t iterations = 0;
    bool one_tree_is_tour = false;  // best candidate 1-tree had all degrees 2
    double ascent_s = 0, exact_s = 0;
};

// Subgradient ascent (deterministic): Kruskal 1-trees on the candidate graph, degree subgradient,
// Polyak step toward ub with decay (lambda halves after `patience` stalls). Then the valid bound.
HkResult held_karp(const Instance& inst, const CandidateGraph& g, int64_t ub, const HkConfig& cfg);

// Exact minimum 1-tree value L(pi) over the complete graph (dense Prim on nodes 1..n-1 plus the
// two cheapest edges at node 0). Valid lower bound on the optimal tour for every pi.
double one_tree_bound_complete(const Instance& inst, const std::vector<double>& pi);

}  // namespace apex
