#pragma once

#include <cstdint>
#include <vector>

#include "bnc/root_loop.hpp"

namespace apex {

// Reduced-cost fixing, MASTER §5.5. `bound` must be globally valid for the scope it is used in
// (root: the root Farley bound; node: that node's Farley bound, valid in its subtree only).
// Sound integral rule: x_e = 0 iff ceil(bound + d_e - 1e-6) >= ub (nonbasic at lower, d_e > 0);
// x_e = 1 iff ceil(bound - d_e - 1e-6) >= ub (nonbasic at upper, d_e < 0). Returns every LP column
// of `res` that qualifies (callers skip the ones already fixed).
std::vector<BoundChange> rcf_fixings(const SimplexResult& res, double bound, int64_t ub);

// Root scope: permanent, recorded in g.edges[].status (kFixed0 | kRcfPruned, kFixed1).
void apply_root_rcf(CandidateGraph& g, const SimplexResult& final, double global_lb, int64_t ub,
                    uint32_t& fixed0, uint32_t& fixed1);

}  // namespace apex
