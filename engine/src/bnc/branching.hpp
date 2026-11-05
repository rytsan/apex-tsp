#pragma once

#include <cstdint>
#include <vector>

#include "lp/dual_simplex.hpp"

namespace apex {

struct BranchingConfig {
    bool strong = true;              // false: pseudocost / most-fractional only
    uint32_t reliability = 4;        // observations per direction before pseudocosts are trusted
    uint32_t top_k = 8;              // strong-branching candidates (closest to 0.5)
    uint32_t pivots_per_child = 10;  // simulated dual pivots per child LP
    uint32_t node_budget = 60;       // simulated pivots per node, all candidates together
};

// Per-edge objective gain per unit of fractionality change, down (x_e -> 0, change x_e) and
// up (x_e -> 1, change 1 - x_e). Grows with the edge set (priced columns).
class Pseudocosts {
public:
    void observe(uint32_t e, int dir, double gain_per_unit);
    uint32_t count(uint32_t e, int dir) const { return e < cnt_[dir].size() ? cnt_[dir][e] : 0; }
    // Mean for e; the global mean of that direction when e has no observation (1 if none at all).
    double get(uint32_t e, int dir) const;

private:
    std::vector<double> sum_[2];
    std::vector<uint32_t> cnt_[2];
    double total_[2] = {0, 0};
    uint64_t total_cnt_[2] = {0, 0};
};

struct BranchDecision {
    int64_t edge = -1;               // -1: no fractional free column
    double score = 0;
    uint32_t strong_candidates = 0;  // candidates evaluated by strong branching
    uint64_t strong_pivots = 0;      // simulated pivots spent (<= node_budget)
};

// Reliability branching. Every fractional free column gets the product score
// max(gain_down, 1e-6) * max(gain_up, 1e-6). Among the top_k closest to 0.5, those with fewer
// than `reliability` observations in some direction are evaluated by strong branching (child
// LPs from `basis`, at most pivots_per_child dual pivots each, node_budget in total) and the
// results enter the pseudocosts; the rest use pseudocosts. Ties go to the lowest edge index.
// lp: the node LP (bounds are changed and restored in place).
BranchDecision select_branch(Lp& lp, const Basis& basis, const SimplexResult& res, const SimplexOptions& opt,
                             Pseudocosts& pc, const BranchingConfig& cfg);

}  // namespace apex
