#include "bnc/rcf.hpp"

#include <cmath>

namespace apex {

std::vector<BoundChange> rcf_fixings(const SimplexResult& res, double bound, int64_t ub) {
    std::vector<BoundChange> out;
    if (!std::isfinite(bound) || ub < 0) return out;
    const double u = static_cast<double>(ub);
    for (uint32_t j = 0; j < res.var_state.size(); ++j) {
        const auto st = static_cast<VarState>(res.var_state[j]);
        const double d = res.reduced_costs[j];
        if (st == VarState::AtLower && d > 0 && std::ceil(bound + d - 1e-6) >= u) out.push_back({j, 0});
        else if (st == VarState::AtUpper && d < 0 && std::ceil(bound - d - 1e-6) >= u) out.push_back({j, 1});
    }
    return out;
}

void apply_root_rcf(CandidateGraph& g, const SimplexResult& final, double global_lb, int64_t ub, uint32_t& fixed0,
                    uint32_t& fixed1) {
    for (const auto& f : rcf_fixings(final, global_lb, ub)) {
        auto& st = g.edges[f.edge].status;
        if (st & (kFixed0 | kFixed1)) continue;
        if (f.value == 0) { st |= kFixed0 | kRcfPruned; ++fixed0; }
        else { st |= kFixed1; ++fixed1; }
    }
}

}  // namespace apex
