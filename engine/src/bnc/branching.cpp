#include "bnc/branching.hpp"

#include <algorithm>
#include <cmath>

namespace apex {

void Pseudocosts::observe(uint32_t e, int dir, double gain_per_unit) {
    if (!(gain_per_unit >= 0)) gain_per_unit = 0;  // numerical noise / NaN
    if (sum_[dir].size() <= e) {
        sum_[dir].resize(e + 1, 0.0);
        cnt_[dir].resize(e + 1, 0);
    }
    sum_[dir][e] += gain_per_unit;
    ++cnt_[dir][e];
    total_[dir] += gain_per_unit;
    ++total_cnt_[dir];
}

double Pseudocosts::get(uint32_t e, int dir) const {
    if (count(e, dir) > 0) return sum_[dir][e] / cnt_[dir][e];
    return total_cnt_[dir] > 0 ? total_[dir] / static_cast<double>(total_cnt_[dir]) : 1.0;
}

BranchDecision select_branch(Lp& lp, const Basis& basis, const SimplexResult& res, const SimplexOptions& opt,
                             Pseudocosts& pc, const BranchingConfig& cfg) {
    BranchDecision out;
    std::vector<uint32_t> cand;
    for (uint32_t j = 0; j < res.x.size(); ++j)
        if (lp.lower[j] != lp.upper[j] && res.x[j] > 1e-6 && res.x[j] < 1 - 1e-6) cand.push_back(j);
    if (cand.empty()) return out;
    std::stable_sort(cand.begin(), cand.end(), [&](uint32_t a, uint32_t b) {
        return std::abs(res.x[a] - 0.5) < std::abs(res.x[b] - 0.5);  // stable: ties keep index order
    });

    auto score = [](double down, double up) { return std::max(down, 1e-6) * std::max(up, 1e-6); };
    std::vector<double> sc(cand.size());
    for (size_t k = 0; k < cand.size(); ++k) {
        const uint32_t j = cand[k];
        sc[k] = score(pc.get(j, 0) * res.x[j], pc.get(j, 1) * (1 - res.x[j]));
    }

    if (cfg.strong) {
        uint64_t left = cfg.node_budget;
        for (size_t k = 0; k < std::min<size_t>(cfg.top_k, cand.size()) && left > 0; ++k) {
            const uint32_t j = cand[k];
            if (pc.count(j, 0) >= cfg.reliability && pc.count(j, 1) >= cfg.reliability) continue;
            double gain[2] = {pc.get(j, 0) * res.x[j], pc.get(j, 1) * (1 - res.x[j])};  // fallback if the budget ends
            const double lo = lp.lower[j], up = lp.upper[j];
            for (int dir = 0; dir < 2 && left > 0; ++dir) {
                SimplexOptions o = opt;
                o.iteration_limit = std::min<uint64_t>(cfg.pivots_per_child, left);
                lp.lower[j] = lp.upper[j] = dir;
                const auto child = solve_dual(lp, o, &basis);
                left -= std::min<uint64_t>(child.iterations, left);
                out.strong_pivots += child.iterations;
                // Dual objective after a few pivots: a lower estimate of the child LP. An
                // infeasible child (on the candidate graph) scores as a large gain.
                gain[dir] = child.status == SimplexResult::Status::Infeasible ? 1e9 : std::max(0.0, child.objective - res.objective);
                const double frac = dir == 0 ? res.x[j] : 1 - res.x[j];
                if (child.status != SimplexResult::Status::Infeasible) pc.observe(j, dir, gain[dir] / frac);
            }
            lp.lower[j] = lo;
            lp.upper[j] = up;
            sc[k] = score(gain[0], gain[1]);
            ++out.strong_candidates;
        }
    }

    size_t best = 0;
    for (size_t k = 1; k < cand.size(); ++k)
        if (sc[k] > sc[best] || (sc[k] == sc[best] && cand[k] < cand[best])) best = k;
    out.edge = cand[best];
    out.score = sc[best];
    return out;
}

}  // namespace apex
