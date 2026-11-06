#include "bnc/primal_heur.hpp"

#include <utility>

#include "bnc/tree.hpp"
#include "primal/lp_tour.hpp"

namespace apex {

DiveResult dive(const Instance& inst, const CandidateGraph& g, Lp& lp, const Basis& basis, const std::vector<double>& x,
                const SimplexOptions& opt, const DiveConfig& cfg) {
    DiveResult out;
    struct Saved {
        uint32_t j;
        double lower, upper;
    };
    std::vector<Saved> saved;
    std::vector<double> cur = x;
    Basis b = basis;
    SimplexOptions o = opt;
    o.iteration_limit = cfg.pivots_per_round;
    bool backtrack_used = false, retry = false;  // one backtrack per dive: infeasible x_e = 1 -> x_e = 0
    int64_t pick = -1;
    while (out.rounds < cfg.max_rounds) {
        if (!retry) {
            pick = -1;
            for (size_t j = 0; j < cur.size(); ++j) {
                if (lp.lower[j] == lp.upper[j]) continue;
                if (cur[j] > 1 - 1e-6) {
                    saved.push_back({static_cast<uint32_t>(j), lp.lower[j], lp.upper[j]});
                    lp.lower[j] = 1;
                } else if (cur[j] > 1e-6 && (pick < 0 || cur[j] > cur[static_cast<size_t>(pick)])) {
                    pick = static_cast<int64_t>(j);
                }
            }
            if (pick < 0) break;  // integral already: nothing to dive on
            const auto p = static_cast<size_t>(pick);
            saved.push_back({static_cast<uint32_t>(p), lp.lower[p], lp.upper[p]});
            lp.lower[p] = 1;
        }
        retry = false;
        ++out.rounds;
        const auto r = solve_dual(lp, o, &b);
        out.pivots += r.iterations;
        if (r.status == SimplexResult::Status::Infeasible && !backtrack_used) {
            const auto p = static_cast<size_t>(pick);
            lp.lower[p] = 0;
            lp.upper[p] = 0;
            backtrack_used = retry = true;
            continue;
        }
        if (r.status != SimplexResult::Status::Optimal) break;
        cur = r.x;
        b = r.basis;
        bool integral = true;
        for (double v : cur)
            if (v > 1e-6 && v < 1 - 1e-6) { integral = false; break; }
        if (integral) {
            out.tour = extract_tour(inst, g, cur);
            break;
        }
    }
    for (size_t k = saved.size(); k-- > 0;) {
        lp.lower[saved[k].j] = saved[k].lower;
        lp.upper[saved[k].j] = saved[k].upper;
    }
    // The restricted dive LP has no new SECs, so integral solutions are often subtours and
    // fixings soon make it infeasible: complete the deepest feasible dive solution greedily.
    if (out.tour.order.empty() && cur != x) {
        out.tour = lp_guided_tour(inst, g, cur);
        if (!is_valid_tour(out.tour, inst.n)) out.tour = Tour{};
    } else if (!out.tour.order.empty()) {
        out.integral = true;
    }
    return out;
}

Tour rih_lite(const Instance& inst, const CandidateGraph& g, const std::vector<double>& x, const Tour& incumbent,
              const IlsConfig& ils) {
    std::vector<double> ones(x.size(), 0.0);
    for (size_t j = 0; j < x.size(); ++j)
        if (x[j] > 1 - 1e-6) ones[j] = 1;
    Tour t = lp_guided_tour(inst, g, ones);  // x = 1 fragments first, greedy reconnection, 2-opt
    if (!is_valid_tour(t, inst.n)) return incumbent;
    iterated_local_search(t, inst, g, ils);
    t.cost = tour_cost(inst, t);
    return t.cost < incumbent.cost ? t : incumbent;
}

}  // namespace apex

