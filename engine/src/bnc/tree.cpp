#include "bnc/tree.hpp"

#include "bnc/rcf.hpp"
#include "core/tour_io.hpp"
#include "primal/lp_tour.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace apex {
namespace {

using Clock = std::chrono::steady_clock;
double secs(Clock::time_point a) { return std::chrono::duration<double>(Clock::now() - a).count(); }

std::string fmt(double v, int p = 4) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", p, v);
    return b;
}

bool integral(const std::vector<double>& x) {
    return std::all_of(x.begin(), x.end(), [](double v) { return v <= 1e-6 || v >= 1 - 1e-6; });
}

bool prunable(double bound, int64_t ub) { return std::ceil(bound - 1e-6) >= static_cast<double>(ub); }

}  // namespace

const char* status_name(BnCResult::Status s) {
    switch (s) {
        case BnCResult::Status::Optimal: return "OPTIMAL";
        case BnCResult::Status::TimeLimit: return "TIME_LIMIT";
        case BnCResult::Status::NodeLimit: return "NODE_LIMIT";
    }
    return "?";
}

Tour extract_tour(const Instance& inst, const CandidateGraph& g, const std::vector<double>& x) {
    const uint32_t n = inst.n;
    std::vector<std::array<uint32_t, 2>> nb(n, {UINT32_MAX, UINT32_MAX});
    for (size_t j = 0; j < x.size(); ++j) {
        if (x[j] < 0.5) continue;
        for (uint32_t a : {g.edges[j].u, g.edges[j].v}) {
            const uint32_t b = a == g.edges[j].u ? g.edges[j].v : g.edges[j].u;
            if (nb[a][0] == UINT32_MAX) nb[a][0] = b;
            else if (nb[a][1] == UINT32_MAX) nb[a][1] = b;
            else return {};  // degree > 2
        }
    }
    Tour t;
    t.order.reserve(n);
    for (uint32_t prev = UINT32_MAX, cur = 0; t.order.size() < n;) {
        t.order.push_back(cur);
        if (nb[cur][1] == UINT32_MAX) return {};  // degree < 2
        const uint32_t next = nb[cur][0] != prev ? nb[cur][0] : nb[cur][1];
        prev = cur;
        cur = next;
        if (cur == 0) break;
    }
    if (!is_valid_tour(t, n)) return {};  // closed early: a subtour
    t.cost = tour_cost(inst, t);           // never trust the LP sum for the UB
    return t;
}

BnCResult run_bnc(const Instance& inst, CandidateGraph& g, const BnCConfig& cfg, Tour initial, TelemetrySink* tel) {
    const auto t0 = Clock::now();
    BnCResult out;
    out.best = std::move(initial);
    out.best.cost = tour_cost(inst, out.best);
    out.ub = out.best.cost;
    if (!cfg.tour_path.empty()) write_tour_file(cfg.tour_path, inst, out.best, "apex_tsp initial ub " + std::to_string(out.ub));
    CutLoop loop(inst, g, cfg.root, tel);
    auto accumulate = [&](const RootLoopResult& r) {
        out.lp_s += r.lp_s;
        out.sep_s += r.sep_s;
        out.pricing_s += r.pricing_s;
    };
    bool found_integral = false;
    bool rcf_pending = false;
    bool dive_over = false;    // dive depth limit reached  // UB improved: re-run root RCF at the next node boundary
    // Every UB candidate goes through here: cost recomputed with the metric (never the LP sum),
    // the .tour file rewritten atomically on each improvement.
    auto offer = [&](Tour t, uint32_t node_id, const char* source) {
        if (!is_valid_tour(t, inst.n)) return;
        t.cost = tour_cost(inst, t);
        if (t.cost >= out.ub) return;
        out.ub = t.cost;
        out.best = std::move(t);
        ++out.ub_improvements;
        rcf_pending = true;
        if (!cfg.tour_path.empty())
            write_tour_file(cfg.tour_path, inst, out.best, "apex_tsp " + std::string(source) + " ub " + std::to_string(out.ub));
        if (tel) tel->event("ub_improved", {{"node", std::to_string(node_id)}, {"ub", std::to_string(out.ub)}, {"source", source}});
    };
    auto try_tour = [&](const RootLoopResult& r, uint32_t node_id) {
        if (r.infeasible) return false;
        if (!integral(r.final.x)) {
            if (cfg.lp_heuristic) {  // heuristic (UB only): greedy by x + 2-opt/Or-opt
                const auto th = Clock::now();
                offer(lp_guided_tour(inst, g, r.final.x), node_id, "lp_guided");
                out.heuristic_s += secs(th);
                ++out.heuristic_calls;
            }
            return false;
        }
        auto t = extract_tour(inst, g, r.final.x);
        if (t.order.empty()) return false;
        found_integral = true;
        offer(std::move(t), node_id, "lp_integral");
        return true;
    };

    // Root: cut-and-price with the root limits, then permanent root RCF against the initial UB.
    NodeLimits root_lim;
    root_lim.max_rounds = cfg.root.max_rounds;
    root_lim.tailing_window = cfg.root.tailing_window;
    root_lim.tailing_rel = cfg.root.tailing_rel;
    root_lim.time_limit_s = cfg.time_limit_s;
    out.root = loop.solve({}, nullptr, root_lim);
    accumulate(out.root);
    out.root_lb = out.root.global_lb;
    out.nodes = 1;
    out.processing_order.push_back(0);
    try_tour(out.root, 0);
    apply_root_rcf(g, out.root.final, out.root.global_lb, out.ub, out.root.rcf_fixed0, out.root.rcf_fixed1);

    std::vector<BnCNode> open;
    Pseudocosts pc;
    uint32_t next_id = 1;
    auto branch = [&](const RootLoopResult& r, const BnCNode& parent) {
        Lp lp = loop.lp();
        const auto d = select_branch(lp, loop.basis().basis, r.final, cfg.root.lp, pc, cfg.branching);
        out.strong_pivots += d.strong_pivots;
        out.strong_candidates += d.strong_candidates;
        out.max_node_strong_pivots = std::max(out.max_node_strong_pivots, d.strong_pivots);
        const int64_t j = d.edge;
        if (j < 0) return false;
        auto basis = std::make_shared<const NodeBasis>(loop.basis());
        const auto e = static_cast<uint32_t>(j);
        // Node-scoped RCF with this node's Farley bound: valid in its subtree only, so the fixes
        // travel in the children's bound changes (MASTER §5.5 scoping).
        BoundChanges fixes;
        for (const auto& f : rcf_fixings(r.final, r.global_lb, out.ub)) {
            if (f.edge == e || lp.lower[f.edge] == lp.upper[f.edge] || (g.edges[f.edge].status & (kFixed0 | kFixed1))) continue;
            fixes.push_back(f);
        }
        out.rcf_node_fixings += fixes.size();
        for (uint8_t v : {uint8_t{0}, uint8_t{1}}) {  // x_e = 1 pushed last: the dive takes it first
            BnCNode c{next_id++, parent.depth + 1, r.global_lb, parent.bounds, basis, NodeState::Open,
                      j, v, v ? 1 - r.final.x[e] : r.final.x[e], r.lp_value};
            c.bounds.insert(c.bounds.end(), fixes.begin(), fixes.end());
            c.bounds.push_back({e, v});
            open.push_back(std::move(c));
        }
        ++out.branched;
        return true;
    };
    // Node outcome. A tour whose Farley bound is still below the UB was interrupted by the time
    // limit before pricing finished: the node stays open (with its own, valid bound).
    auto decide = [&](const RootLoopResult& r, BnCNode& node) {
        if (r.infeasible) { ++out.infeasible; return; }
        const bool tour = try_tour(r, node.id);
        if (prunable(r.global_lb, out.ub)) { ++(tour ? out.integral : out.pruned); return; }
        if (tour || !std::isfinite(r.global_lb)) {
            node.dual_bound = std::max(node.dual_bound, r.global_lb);
            open.push_back(std::move(node));
            return;
        }
        if (!branch(r, node)) throw std::logic_error("bnc: fractional node without a branching candidate");
    };

    BnCNode root{0, 0, out.root_lb, {}, nullptr, NodeState::Processing};
    if (!std::isfinite(root.dual_bound)) root.dual_bound = -INFINITY;
    bool timed_out = false;
    if (out.root.infeasible) throw std::logic_error("bnc: root LP infeasible over the complete graph");
    decide(out.root, root);

    while (!open.empty()) {
        if (secs(t0) > cfg.time_limit_s) { timed_out = true; break; }
        if (out.nodes >= cfg.max_nodes) break;
        if (rcf_pending) {  // root reduced costs + root bound stay valid globally; the new UB fixes more
            rcf_pending = false;
            const uint32_t f0 = out.root.rcf_fixed0, f1 = out.root.rcf_fixed1;
            apply_root_rcf(g, out.root.final, out.root_lb, out.ub, out.root.rcf_fixed0, out.root.rcf_fixed1);
            if (tel)
                tel->event("rcf_root", {{"ub", std::to_string(out.ub)}, {"new_fixed0", std::to_string(out.root.rcf_fixed0 - f0)},
                                        {"new_fixed1", std::to_string(out.root.rcf_fixed1 - f1)},
                                        {"pruned_edges_rcf", std::to_string(out.root.rcf_fixed0)}});
        }
        size_t pick = open.size() - 1;  // dive: last pushed
        if (found_integral || dive_over || cfg.select == BnCConfig::Select::BestBound) {
            for (size_t k = 0; k < open.size(); ++k)  // ponytail: O(open) scan; heap if fronts grow past ~1e5
                if (open[k].dual_bound < open[pick].dual_bound ||
                    (open[k].dual_bound == open[pick].dual_bound && open[k].id < open[pick].id))
                    pick = k;
        }
        BnCNode node = std::move(open[pick]);
        open.erase(open.begin() + static_cast<std::ptrdiff_t>(pick));
        if (prunable(node.dual_bound, out.ub)) { ++out.pruned; continue; }
        ++out.nodes;
        out.max_depth = std::max<uint64_t>(out.max_depth, node.depth);
        if (node.depth >= cfg.dive_depth_limit) dive_over = true;  // dive went too deep: best-bound from now on
        out.processing_order.push_back(node.id);
        NodeLimits lim = cfg.node;
        lim.cutoff = out.ub;
        lim.time_limit_s = std::max(0.0, cfg.time_limit_s - secs(t0));
        const bool warm_check = node.parent_basis && loop.same_columns(*node.parent_basis);
        auto r = loop.solve(node.bounds, node.parent_basis.get(), lim);
        accumulate(r);
        // Bound changes alone never break dual feasibility of the parent basis (MASTER M3.1).
        if (warm_check && r.first_dual_flips > 0) ++out.warm_dual_infeasible;
        if (node.branch_edge >= 0 && !r.infeasible && (r.converged || r.tailing_off))  // full child LP gain
            pc.observe(static_cast<uint32_t>(node.branch_edge), node.branch_dir,
                       std::max(0.0, r.lp_value - node.parent_lp) / node.branch_frac);
        ++(node.parent_basis ? out.warm_starts : out.cold_starts);
        if (r.cutoff) { ++out.pruned; continue; }
        decide(r, node);
        if (tel && out.nodes % 1000 == 0)
            tel->event("tree_progress", {{"nodes", std::to_string(out.nodes)}, {"open", std::to_string(open.size())},
                                         {"ub", std::to_string(out.ub)}, {"lb", fmt(open.empty() ? out.ub : node.dual_bound)}});
    }

    out.lb = static_cast<double>(out.ub);
    for (const auto& nd : open) out.lb = std::min(out.lb, nd.dual_bound);
    out.lb_int = static_cast<int64_t>(std::ceil(out.lb - 1e-6));
    if (!open.empty() && secs(t0) > cfg.time_limit_s) timed_out = true;
    out.status = open.empty() ? BnCResult::Status::Optimal
                              : (timed_out ? BnCResult::Status::TimeLimit : BnCResult::Status::NodeLimit);
    out.total_s = secs(t0);
    if (tel)
        tel->event("FINISHED", {{"status", status_name(out.status)}, {"ub", std::to_string(out.ub)}, {"lb", fmt(out.lb)},
                                {"gap_pct", fmt(100.0 * (out.ub - out.lb) / std::max(1.0, static_cast<double>(out.ub)))},
                                {"nodes", std::to_string(out.nodes)}, {"open", std::to_string(open.size())},
                                {"max_depth", std::to_string(out.max_depth)}, {"root_lb", fmt(out.root_lb)},
                                {"lp_s", fmt(out.lp_s, 2)}, {"sep_s", fmt(out.sep_s, 2)},
                                {"pricing_s", fmt(out.pricing_s, 2)}, {"total_s", fmt(out.total_s, 2)}});
    return out;
}

}  // namespace apex
