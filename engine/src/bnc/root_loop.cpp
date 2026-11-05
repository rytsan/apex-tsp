#include "bnc/root_loop.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include "cuts/support.hpp"

namespace apex {
namespace {

using Clock = std::chrono::steady_clock;
double secs(Clock::time_point a) { return std::chrono::duration<double>(Clock::now() - a).count(); }

std::string fmt(double v, int p = 3) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", p, v);
    return b;
}

// LP with degree rows x(delta(i)) = 2 plus the given cut rows.
Lp build_lp(const Instance& inst, const CandidateGraph& g, const CutPool& pool, const std::vector<size_t>& active) {
    const uint32_t n = inst.n, m = static_cast<uint32_t>(g.edges.size());
    std::vector<Triplet> t;
    t.reserve(2 * m);
    Lp lp;
    lp.c.resize(m);
    lp.lower.assign(m, 0.0);
    lp.upper.assign(m, 1.0);
    for (uint32_t e = 0; e < m; ++e) {
        t.push_back({g.edges[e].u, e, 1.0});
        t.push_back({g.edges[e].v, e, 1.0});
        lp.c[e] = g.edges[e].cost;
        if (g.edges[e].status & kFixed0) lp.upper[e] = 0.0;
        if (g.edges[e].status & kFixed1) lp.lower[e] = 1.0;
    }
    lp.A = CscMatrix::from_triplets(n, m, t, std::vector<RowData>(n, {Sense::Eq, 2.0}));
    std::vector<Row> rows;
    for (size_t idx : active) rows.push_back(pool.make_row(pool.at(idx), g.edges));
    lp.A.append_rows(rows);
    lp.A.ensure_merged();
    return lp;
}

// Farley-type bound valid for the complete graph: the LP duals extend to every edge e outside
// the candidate graph with bound dual min(0, rc_e) (x_e <= 1 is implied by degree rows), so
// z_cand + sum_{e not cand} min(0, rc_e) is a weak-duality lower bound. rc_e is exact:
//   rc_e = c_e - y_u - y_v - sum_{cuts K crossing e} y_K,
//   sum_K crossing = sigma_u + sigma_v - 2 * sum_{K containing u and v} y_K.
// (Non-candidate edges are never blossom teeth, so their cut coefficient is +1 when crossing.)
// ponytail: O(n^2) pair scan; replace by grid-based pricing (MASTER §5.6, Sprint 3) for n > ~10k.
struct NegEdge { double rc; uint32_t u, v; };

double farley_bound(const Instance& inst, const CandidateGraph& g, const CutPool& pool, const std::vector<size_t>& active,
                    const SimplexResult& res, uint64_t& negative, std::vector<NegEdge>* list = nullptr) {
    const uint32_t n = inst.n;
    std::vector<double> sigma(n, 0.0);
    std::vector<std::vector<std::pair<uint32_t, double>>> member(n);  // (cut k, y_K), sorted by k
    for (size_t k = 0; k < active.size(); ++k) {
        const double y = res.duals[n + k];
        if (y == 0.0) continue;
        for (uint32_t v : pool.at(active[k]).nodes) {
            sigma[v] += y;
            member[v].push_back({static_cast<uint32_t>(k), y});
        }
    }
    std::unordered_set<uint64_t> cand;
    cand.reserve(g.edges.size() * 2);
    for (const auto& e : g.edges) cand.insert((uint64_t(e.u) << 32) | e.v);
    double sum = 0;
    negative = 0;
    for (uint32_t u = 0; u < n; ++u)
        for (uint32_t v = u + 1; v < n; ++v) {
            double rc = dist(inst, u, v) - res.duals[u] - res.duals[v] - sigma[u] - sigma[v];
            if (rc >= 0) continue;  // the both-endpoints term only increases rc
            const auto& a = member[u];
            const auto& b = member[v];
            double both = 0;
            for (size_t i = 0, j = 0; i < a.size() && j < b.size();) {
                if (a[i].first == b[j].first) { both += a[i].second; ++i; ++j; }
                else if (a[i].first < b[j].first) ++i;
                else ++j;
            }
            rc += 2 * both;
            if (rc >= -1e-9 || cand.count((uint64_t(u) << 32) | v)) continue;
            sum += rc;
            ++negative;
            if (list) list->push_back({rc, u, v});
        }
    return res.objective + sum;
}

}  // namespace

RootLoopResult run_root_loop(const Instance& inst, CandidateGraph& g, const RootLoopConfig& cfg, TelemetrySink* tel) {
    const auto t0 = Clock::now();
    const uint32_t n = inst.n;
    RootLoopResult out;
    CutPool pool(cfg.age_limit);
    std::vector<size_t> active;  // pool index of LP row n + k
    out.lp = build_lp(inst, g, pool, active);
    Basis basis;
    bool warm = false;
    double prev_obj = -INFINITY;
    bool added_columns = false;  // previous round added priced columns (LP value may drop)
    // Exact pricing over the complete graph; adds the most negative columns. Returns #added.
    auto price = [&](const SimplexResult& res, double& glb) -> size_t {
        const auto tp = Clock::now();
        std::vector<NegEdge> neg;
        uint64_t cnt = 0;
        glb = farley_bound(inst, g, pool, active, res, cnt, &neg);
        out.pricing_s += secs(tp);
        if (neg.empty() || !cfg.price || out.pricing_rounds >= cfg.max_pricing_rounds) return 0;
        std::sort(neg.begin(), neg.end(), [](const NegEdge& x, const NegEdge& y) {
            return x.rc < y.rc || (x.rc == y.rc && std::make_pair(x.u, x.v) < std::make_pair(y.u, y.v));
        });
        if (neg.size() > cfg.pricing_batch) neg.resize(cfg.pricing_batch);
        for (const auto& e : neg) g.edges.push_back(Edge{e.u, e.v, dist(inst, e.u, e.v), kActive | kNewPricing, 0.0});
        rebuild_adjacency(g);
        basis.col.resize(g.edges.size(), VarState::AtLower);  // new columns enter at lower bound
        ++out.pricing_rounds;
        out.priced_edges += neg.size();
        if (tel)
            tel->event("root_pricing", {{"round", std::to_string(out.pricing_rounds)}, {"negative", std::to_string(cnt)},
                                        {"added", std::to_string(neg.size())}, {"edges", std::to_string(g.edges.size())},
                                        {"global_lb", fmt(glb, 4)}});
        return neg.size();
    };

    for (uint32_t round = 0;; ++round) {
        RootRoundLog log{round, 0, -INFINITY, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        auto tl = Clock::now();
        auto res = solve_dual(out.lp, cfg.lp, warm ? &basis : nullptr);
        log.lp_s = secs(tl);
        out.lp_s += log.lp_s;
        out.status = res.status;
        if (res.status != SimplexResult::Status::Optimal && res.status != SimplexResult::Status::StallRecovered) {
            out.final = std::move(res);
            break;
        }
        // Debug assertions of the warm-start contract (MASTER M2.6).
        if (res.used_artificial_bounds) throw std::logic_error("root loop: artificial bounds used on the TSP LP");
        if (!added_columns && res.objective < prev_obj - 1e-6 * (1.0 + std::abs(prev_obj)))
            throw std::logic_error("root loop: LP objective decreased across re-solves without new columns");
        prev_obj = res.objective;
        basis = res.basis;
        warm = true;
        log.lp_value = res.objective;
        log.lp_iterations = res.iterations;

        // Aging: only non-binding cuts (basic logical => zero dual) leave the LP, so removing
        // them keeps x, the duals and the basis count consistent.
        auto aged = pool.age(res.duals);
        std::vector<uint8_t> drop(active.size(), 0);
        for (size_t idx : aged) {
            const int32_t row = pool.at(idx).lp_row;
            if (basis.row[row] == VarState::Basic) drop[row - n] = 1;
        }
        if (std::count(drop.begin(), drop.end(), 1) > 0) {
            std::vector<size_t> kept;
            std::vector<VarState> rows(basis.row.begin(), basis.row.begin() + n);
            for (size_t k = 0; k < active.size(); ++k) {
                if (drop[k]) { pool.set_lp_row(active[k], -1); ++log.aged_out; continue; }
                pool.set_lp_row(active[k], static_cast<int32_t>(n + kept.size()));
                rows.push_back(basis.row[n + k]);
                kept.push_back(active[k]);
            }
            active.swap(kept);
            basis.row.swap(rows);
            out.lp = build_lp(inst, g, pool, active);
        }

        if (round >= cfg.max_rounds || secs(t0) > cfg.time_limit_s) { out.final = std::move(res); break; }
        if (cfg.tailing_window > 0 && out.rounds.size() >= cfg.tailing_window) {
            const double old = out.rounds[out.rounds.size() - cfg.tailing_window].lp_value;
            if (res.objective - old < cfg.tailing_rel * std::abs(res.objective)) {
                out.tailing_off = true;
                out.final = std::move(res);
                break;
            }
        }

        // Separation.
        const auto ts = Clock::now();
        const auto sup = build_support(n, g.edges, res.x);
        std::vector<std::pair<double, size_t>> cand;  // (violation, pool index)
        for (size_t idx : pool.violated_outside(g.edges, res.x)) cand.push_back({pool.violation(pool.at(idx), g.edges, res.x), idx});
        log.reactivated = static_cast<uint32_t>(cand.size());
        SecSeparationStats sst;
        const auto secs_found = separate_sec(sup, cfg.sec, &sst);
        for (const auto& c : secs_found) pool.stage_sec(c);
        std::vector<BlossomCut> bl;
        if (cfg.use_blossom && secs_found.empty()) bl = separate_blossom(sup, cfg.blossom);
        for (const auto& c : bl) pool.stage_blossom(c);
        for (size_t idx : pool.commit()) {
            const double v = pool.violation(pool.at(idx), g.edges, res.x);  // exact on LP columns
            if (v > cfg.sec.violation_tol) cand.push_back({v, idx});
        }
        log.new_component = sst.component_cuts;
        log.new_sec = sst.stoer_wagner_cuts + sst.pairwise_cuts;
        log.new_blossom = static_cast<uint32_t>(bl.size());
        log.sep_s = secs(ts);
        out.sep_s += log.sep_s;
        std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
        if (cand.size() > cfg.max_cuts_per_round) cand.resize(cfg.max_cuts_per_round);
        log.rows = static_cast<uint32_t>(out.lp.A.num_rows() + cand.size());
        out.cuts_component += log.new_component;
        out.cuts_sec += log.new_sec;
        out.cuts_blossom += log.new_blossom;
        pool.note_reactivated(log.reactivated);
        out.rounds.push_back(log);
        if (tel)
            tel->event("root_round", {{"round", std::to_string(round)}, {"lp", fmt(log.lp_value, 4)},
                                      {"rows", std::to_string(log.rows)}, {"comp", std::to_string(log.new_component)},
                                      {"sec", std::to_string(log.new_sec)}, {"blossom", std::to_string(log.new_blossom)},
                                      {"pool", std::to_string(log.reactivated)}, {"aged", std::to_string(log.aged_out)},
                                      {"iters", std::to_string(log.lp_iterations)}, {"lp_s", fmt(log.lp_s)},
                                      {"sep_s", fmt(log.sep_s)}});
        double glb = -INFINITY;
        const size_t new_cols = cfg.compute_global_bound ? price(res, glb) : 0;
        out.rounds.back().global_lb = glb;
        added_columns = new_cols > 0;
        if (cand.empty() && new_cols == 0) {
            out.converged = true;
            out.final = std::move(res);
            break;
        }
        if (new_cols > 0 && cand.empty()) {  // only columns: rebuild LP (rows unchanged) and re-solve
            out.lp = build_lp(inst, g, pool, active);
            continue;
        }
        std::vector<Row> rows;
        for (const auto& [v, idx] : cand) {
            rows.push_back(pool.make_row(pool.at(idx), g.edges));
            pool.set_lp_row(idx, static_cast<int32_t>(n + active.size()));
            active.push_back(idx);
            basis.row.push_back(VarState::Basic);  // new >= rows: logical basic keeps dual feasibility
        }
        if (new_cols > 0) {  // new columns: rebuild with them, then append the new cut rows
            out.lp = build_lp(inst, g, pool, active);
            // rows were already registered in `active` above; build_lp included them
        } else {
            out.lp.A.append_rows(rows);
            out.lp.A.ensure_merged();
        }
    }

    out.lp_value = out.final.objective;
    out.pool = pool.stats();
    if (cfg.compute_global_bound && !out.final.duals.empty()) {
        const auto tp = Clock::now();
        out.global_lb = farley_bound(inst, g, pool, active, out.final, out.negative_rc_edges);
        out.global_lb_int = static_cast<int64_t>(std::ceil(out.global_lb - 1e-6));
        out.pricing_s += secs(tp);

        // Root reduced-cost fixing with the sound integral rule (see header).
        if (cfg.ub >= 0) {
            for (uint32_t j = 0; j < g.edges.size(); ++j) {
                const auto st = static_cast<VarState>(out.final.var_state[j]);
                const double d = out.final.reduced_costs[j];
                if (st == VarState::AtLower && d > 0 && !(g.edges[j].status & kFixed0) &&
                    std::ceil(out.global_lb + d - 1e-6) >= static_cast<double>(cfg.ub)) {
                    g.edges[j].status |= kFixed0 | kRcfPruned;
                    out.lp.upper[j] = 0.0;
                    ++out.rcf_fixed0;
                } else if (st == VarState::AtUpper && d < 0 && !(g.edges[j].status & kFixed1) &&
                           std::ceil(out.global_lb - d - 1e-6) >= static_cast<double>(cfg.ub)) {
                    g.edges[j].status |= kFixed1;
                    out.lp.lower[j] = 1.0;
                    ++out.rcf_fixed1;
                }
            }
        }
    }
    out.total_s = secs(t0);
    if (tel)
        tel->event("root_done", {{"converged", out.converged ? "true" : "false"}, {"tailing_off", out.tailing_off ? "true" : "false"}, {"rounds", std::to_string(out.rounds.size())},
                                 {"lp_value", fmt(out.lp_value, 4)}, {"lp_scope", "candidate_graph"},
                                 {"global_lb", fmt(out.global_lb, 4)}, {"lb_source", "lp"},
                                 {"negative_rc_edges", std::to_string(out.negative_rc_edges)},
                                 {"pricing_rounds", std::to_string(out.pricing_rounds)}, {"priced_edges", std::to_string(out.priced_edges)},
                                 {"cuts_sec", std::to_string(out.cuts_sec)}, {"cuts_blossom", std::to_string(out.cuts_blossom)},
                                 {"cuts_component", std::to_string(out.cuts_component)},
                                 {"rcf_fixed0", std::to_string(out.rcf_fixed0)}, {"rcf_fixed1", std::to_string(out.rcf_fixed1)},
                                 {"lp_s", fmt(out.lp_s)}, {"sep_s", fmt(out.sep_s)}, {"pricing_s", fmt(out.pricing_s)},
                                 {"total_s", fmt(out.total_s)}});
    return out;
}

}  // namespace apex
