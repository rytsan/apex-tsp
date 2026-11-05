#include "bnc/root_loop.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "bnc/rcf.hpp"
#include "core/serial.hpp"
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

// Farley-type bound valid for the complete graph: the LP duals extend to every edge e outside
// the candidate graph with bound dual min(0, rc_e) (x_e <= 1 is implied by degree rows), so
// z_cand + sum_{e not cand} min(0, rc_e) is a weak-duality lower bound. rc_e is exact over all
// n(n-1)/2 pairs (EdgePricer: spatial grid with an exact radius, MASTER §5.6).
// farkas: rc_e = -f^T a_e with the Farkas ray f in place of y and zero costs; an edge with
// rc_e < 0 is the only kind that can repair an infeasible LP.
double farley_bound(EdgePricer& pricer, PricingMode mode, const Instance& inst, const CandidateGraph& g,
                    const CutPool& pool, const std::vector<size_t>& active, const SimplexResult& res,
                    uint64_t& negative, std::vector<NegEdge>* list = nullptr, bool farkas = false) {
    const auto& y = farkas ? res.farkas : res.duals;
    const uint32_t n = inst.n;
    std::vector<CutDual> cuts(active.size());
    for (size_t k = 0; k < active.size(); ++k) cuts[k] = {&pool.at(active[k]).nodes, y[n + k]};
    std::unordered_set<uint64_t> cand;
    cand.reserve(g.edges.size() * 2);
    for (const auto& e : g.edges) cand.insert((uint64_t(e.u) << 32) | e.v);
    const std::vector<double> ynode(y.begin(), y.begin() + n);
    const double sum = pricer.price(ynode, cuts, cand, farkas, mode, list, negative);
    return farkas ? 0.0 : res.objective + sum;
}

}  // namespace


CutLoop::CutLoop(const Instance& inst, CandidateGraph& g, const RootLoopConfig& cfg, TelemetrySink* tel)
    : inst_(inst), g_(g), cfg_(cfg), tel_(tel), pool_(cfg.age_limit), pricer_(inst) {
    build_lp();
}

CutLoop::CutLoop(const CutLoop& o, CandidateGraph& g)
    : inst_(o.inst_), g_(g), cfg_(o.cfg_), tel_(nullptr), pool_(o.pool_), pricer_(o.pricer_), active_(o.active_),
      basis_(o.basis_), warm_(o.warm_) {
    build_lp();
}

void CutLoop::reset_to(const NodeBasis& b) {
    for (size_t idx : active_) pool_.set_lp_row(idx, -1);
    active_ = b.active;
    for (size_t k = 0; k < active_.size(); ++k) pool_.set_lp_row(active_[k], static_cast<int32_t>(inst_.n + k));
    basis_ = b.basis;
    basis_.col.resize(g_.edges.size(), VarState::AtLower);
    warm_ = true;
    Lp empty;  // the master never solves in parallel mode: release its LP (solve() rebuilds it)
    std::swap(lp_, empty);
}

void CutLoop::save(std::ostream& o) const {
    using namespace serial;
    pool_.save(o);
    tag(o, "loop");
    put_vec(o, std::vector<uint64_t>(active_.begin(), active_.end()));
    std::vector<uint8_t> col, row;
    for (auto s : basis_.col) col.push_back(static_cast<uint8_t>(s));
    for (auto s : basis_.row) row.push_back(static_cast<uint8_t>(s));
    put_vec(o, col);
    put_vec(o, row);
    put(o, warm_);
    o << '\n';
}

void CutLoop::load(std::istream& in) {
    using namespace serial;
    pool_.load(in);
    expect(in, "loop");
    const auto a = get_vec<uint64_t>(in);
    active_.assign(a.begin(), a.end());
    basis_.col.clear();
    basis_.row.clear();
    for (auto s : get_vec<uint8_t>(in)) basis_.col.push_back(static_cast<VarState>(s));
    for (auto s : get_vec<uint8_t>(in)) basis_.row.push_back(static_cast<VarState>(s));
    warm_ = get<bool>(in);
    build_lp();
}

// LP with degree rows x(delta(i)) = 2 plus the active cut rows; bounds from apply_bounds().
void CutLoop::build_lp() {
    const uint32_t n = inst_.n, m = static_cast<uint32_t>(g_.edges.size());
    std::vector<Triplet> t;
    t.reserve(2 * m);
    lp_.c.clear();
    lp_.c.resize(m);
    for (uint32_t e = 0; e < m; ++e) {
        t.push_back({g_.edges[e].u, e, 1.0});
        t.push_back({g_.edges[e].v, e, 1.0});
        lp_.c[e] = g_.edges[e].cost;
    }
    lp_.A = CscMatrix::from_triplets(n, m, t, std::vector<RowData>(n, {Sense::Eq, 2.0}));
    std::vector<Row> rows;
    for (size_t idx : active_) rows.push_back(pool_.make_row(pool_.at(idx), g_.edges));
    lp_.A.append_rows(rows);
    lp_.A.ensure_merged();
    apply_bounds();
}

void CutLoop::apply_bounds() {
    const size_t m = g_.edges.size();
    lp_.lower.assign(m, 0.0);
    lp_.upper.assign(m, 1.0);
    for (size_t e = 0; e < m; ++e) {
        if (g_.edges[e].status & kFixed0) lp_.upper[e] = 0.0;
        if (g_.edges[e].status & kFixed1) lp_.lower[e] = 1.0;
    }
    for (const auto& b : bc_) lp_.lower[b.edge] = lp_.upper[b.edge] = b.value;
}

// Takes over a stored basis. Cut rows that are nonbasic in it are put back into the LP (pool
// indices are stable), rows it does not know enter with a basic logical, rows that were basic in
// it but are gone are dropped from it; the basic count then equals the row count. New columns
// enter at their lower bound.
void CutLoop::adopt(const NodeBasis& warm) {
    const uint32_t n = inst_.n;
    std::unordered_map<size_t, VarState> state;  // pool index -> logical state in `warm`
    for (size_t k = 0; k < warm.active.size(); ++k) state[warm.active[k]] = warm.basis.row[n + k];
    std::unordered_set<size_t> present(active_.begin(), active_.end());
    bool rows_added = false;
    for (size_t idx : warm.active)
        if (state[idx] != VarState::Basic && !present.count(idx)) {
            pool_.set_lp_row(idx, static_cast<int32_t>(n + active_.size()));
            active_.push_back(idx);
            rows_added = true;
        }
    basis_.col = warm.basis.col;
    basis_.col.resize(g_.edges.size(), VarState::AtLower);
    basis_.row.assign(warm.basis.row.begin(), warm.basis.row.begin() + n);
    for (size_t idx : active_) {
        const auto it = state.find(idx);
        basis_.row.push_back(it == state.end() ? VarState::Basic : it->second);
    }
    warm_ = true;
    if (rows_added) build_lp();
}

// Exact pricing over the complete graph; adds the most negative columns. Returns #added.
size_t CutLoop::price(const SimplexResult& res, double& glb, RootLoopResult& out, bool farkas) {
    const auto tp = Clock::now();
    std::vector<NegEdge> neg;
    uint64_t cnt = 0;
    glb = farley_bound(pricer_, cfg_.pricing_mode, inst_, g_, pool_, active_, res, cnt, &neg, farkas);
    out.pricing_s += secs(tp);
    ++out.pricing_calls;
    if (farkas) glb = -INFINITY;  // a ray proves nothing about the bound until it is clean
    if (neg.empty() || !cfg_.price || out.pricing_rounds >= cfg_.max_pricing_rounds) return 0;
    std::sort(neg.begin(), neg.end(), [](const NegEdge& x, const NegEdge& y) {
        return x.rc < y.rc || (x.rc == y.rc && std::make_pair(x.u, x.v) < std::make_pair(y.u, y.v));
    });
    if (neg.size() > cfg_.pricing_batch) neg.resize(cfg_.pricing_batch);
    for (const auto& e : neg) g_.edges.push_back(Edge{e.u, e.v, dist(inst_, e.u, e.v), kActive | kNewPricing, 0.0});
    rebuild_adjacency(g_);
    basis_.col.resize(g_.edges.size(), VarState::AtLower);  // new columns enter at lower bound
    ++out.pricing_rounds;
    out.priced_edges += neg.size();
    if (tel_)
        tel_->event("pricing", {{"round", std::to_string(out.pricing_rounds)}, {"farkas", farkas ? "true" : "false"},
                                {"negative", std::to_string(cnt)}, {"added", std::to_string(neg.size())},
                                {"edges", std::to_string(g_.edges.size())}, {"global_lb", fmt(glb, 4)}});
    return neg.size();
}

RootLoopResult CutLoop::solve(const BoundChanges& bc, const NodeBasis* warm, const NodeLimits& lim) {
    const auto t0 = Clock::now();
    const uint32_t n = inst_.n;
    RootLoopResult out;
    bc_ = bc;
    if (warm) adopt(*warm);
    // Fresh LP every call: the matrix depends only on (graph, active rows), so a run resumed from
    // a checkpoint solves bit-identical LPs (also covers columns priced in by an early stop).
    build_lp();
    double prev_obj = -INFINITY;
    bool added_columns = false;  // previous round added priced columns (LP value may drop)

    for (uint32_t round = 0;; ++round) {
        RootRoundLog log{round, 0, -INFINITY, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        auto tl = Clock::now();
        auto res = solve_dual(lp_, cfg_.lp, warm_ ? &basis_ : nullptr);
        log.lp_s = secs(tl);
        out.lp_s += log.lp_s;
        out.status = res.status;
        if (round == 0) out.first_dual_flips = res.initial_dual_flips;
        if (res.status == SimplexResult::Status::Infeasible) {
            if (res.farkas.empty()) throw std::logic_error("cut loop: infeasible LP without a Farkas ray");
            basis_ = res.basis;
            warm_ = true;
            double unused = 0;
            if (price(res, unused, out, true) > 0) {  // repairing columns exist: add and re-solve
                build_lp();
                added_columns = true;
                prev_obj = -INFINITY;
                continue;
            }
            out.infeasible = true;
            out.global_lb = INFINITY;
            out.final = std::move(res);
            break;
        }
        if (res.status != SimplexResult::Status::Optimal && res.status != SimplexResult::Status::StallRecovered) {
            out.final = std::move(res);
            break;
        }
        // Debug assertions of the warm-start contract (MASTER M2.6).
        if (res.used_artificial_bounds) throw std::logic_error("cut loop: artificial bounds used on the TSP LP");
        if (!added_columns && res.objective < prev_obj - 1e-6 * (1.0 + std::abs(prev_obj)))
            throw std::logic_error("cut loop: LP objective decreased across re-solves without new columns");
        prev_obj = res.objective;
        basis_ = res.basis;
        warm_ = true;
        log.lp_value = res.objective;
        log.lp_iterations = res.iterations;

        // Aging: only non-binding cuts (basic logical => zero dual) leave the LP, so removing
        // them keeps x, the duals and the basis count consistent.
        auto aged = pool_.age(res.duals);
        std::vector<uint8_t> drop(active_.size(), 0);
        for (size_t idx : aged) {
            const int32_t row = pool_.at(idx).lp_row;
            if (basis_.row[row] == VarState::Basic) drop[row - n] = 1;
        }
        if (std::count(drop.begin(), drop.end(), 1) > 0) {
            std::vector<size_t> kept;
            std::vector<VarState> rows(basis_.row.begin(), basis_.row.begin() + n);
            for (size_t k = 0; k < active_.size(); ++k) {
                if (drop[k]) { pool_.set_lp_row(active_[k], -1); ++log.aged_out; continue; }
                pool_.set_lp_row(active_[k], static_cast<int32_t>(n + kept.size()));
                rows.push_back(basis_.row[n + k]);
                kept.push_back(active_[k]);
            }
            active_.swap(kept);
            basis_.row.swap(rows);
            build_lp();
        }

        // Round and tailing limits apply only to a fractional x: an integral non-tour x always
        // has a violated component cut and must be cut off before the node can be decided.
        const bool fractional = std::any_of(res.x.begin(), res.x.end(), [](double v) { return v > 1e-6 && v < 1 - 1e-6; });
        if (secs(t0) > lim.time_limit_s || (fractional && round >= lim.max_rounds)) { out.final = std::move(res); break; }
        if (fractional && lim.tailing_window > 0 && out.rounds.size() >= lim.tailing_window) {
            const double old = out.rounds[out.rounds.size() - lim.tailing_window].lp_value;
            if (res.objective - old < lim.tailing_rel * std::abs(res.objective)) {
                out.tailing_off = true;
                out.final = std::move(res);
                break;
            }
        }

        // Separation.
        const auto ts = Clock::now();
        const auto sup = build_support(n, g_.edges, res.x);
        std::vector<std::pair<double, size_t>> cand;  // (violation, pool index)
        for (size_t idx : pool_.violated_outside(g_.edges, res.x)) cand.push_back({pool_.violation(pool_.at(idx), g_.edges, res.x), idx});
        log.reactivated = static_cast<uint32_t>(cand.size());
        SecSeparationStats sst;
        const auto secs_found = separate_sec(sup, cfg_.sec, &sst);
        for (const auto& c : secs_found) pool_.stage_sec(c);
        std::vector<BlossomCut> bl;
        if (cfg_.use_blossom && secs_found.empty()) bl = separate_blossom(sup, cfg_.blossom);
        for (const auto& c : bl) pool_.stage_blossom(c);
        for (size_t idx : pool_.commit()) {
            const double v = pool_.violation(pool_.at(idx), g_.edges, res.x);  // exact on LP columns
            if (v > cfg_.sec.violation_tol) cand.push_back({v, idx});
        }
        log.new_component = sst.component_cuts;
        log.new_sec = sst.stoer_wagner_cuts + sst.pairwise_cuts;
        log.new_blossom = static_cast<uint32_t>(bl.size());
        log.sep_s = secs(ts);
        out.sep_s += log.sep_s;
        std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
        if (cand.size() > cfg_.max_cuts_per_round) cand.resize(cfg_.max_cuts_per_round);
        log.rows = static_cast<uint32_t>(lp_.A.num_rows() + cand.size());
        out.cuts_component += log.new_component;
        out.cuts_sec += log.new_sec;
        out.cuts_blossom += log.new_blossom;
        pool_.note_reactivated(log.reactivated);
        out.rounds.push_back(log);
        if (tel_)
            tel_->event("cut_round", {{"round", std::to_string(round)}, {"lp", fmt(log.lp_value, 4)},
                                      {"rows", std::to_string(log.rows)}, {"comp", std::to_string(log.new_component)},
                                      {"sec", std::to_string(log.new_sec)}, {"blossom", std::to_string(log.new_blossom)},
                                      {"pool", std::to_string(log.reactivated)}, {"aged", std::to_string(log.aged_out)},
                                      {"iters", std::to_string(log.lp_iterations)}, {"lp_s", fmt(log.lp_s)},
                                      {"sep_s", fmt(log.sep_s)}});
        double glb = -INFINITY;
        const size_t new_cols = cfg_.compute_global_bound ? price(res, glb, out, false) : 0;
        out.rounds.back().global_lb = glb;
        added_columns = new_cols > 0;
        if (cand.empty() && new_cols == 0) {
            out.converged = true;
            out.final = std::move(res);
            break;
        }
        if (lim.cutoff >= 0 && std::ceil(glb - 1e-6) >= static_cast<double>(lim.cutoff)) {
            out.cutoff = true;  // the subtree cannot hold a tour better than the cutoff
            out.final = std::move(res);
            break;
        }
        for (const auto& [v, idx] : cand) {
            pool_.set_lp_row(idx, static_cast<int32_t>(n + active_.size()));
            active_.push_back(idx);
            basis_.row.push_back(VarState::Basic);  // new >= rows: logical basic keeps dual feasibility
        }
        if (new_cols > 0) {
            build_lp();  // new columns (and the rows registered above)
        } else {
            std::vector<Row> rows;
            for (const auto& [v, idx] : cand) rows.push_back(pool_.make_row(pool_.at(idx), g_.edges));
            lp_.A.append_rows(rows);
            lp_.A.ensure_merged();
        }
    }

    out.lp_value = out.final.objective;
    out.pool = pool_.stats();
    if (!out.infeasible && cfg_.compute_global_bound && !out.final.duals.empty()) {
        const auto tp = Clock::now();
        out.global_lb = farley_bound(pricer_, cfg_.pricing_mode, inst_, g_, pool_, active_, out.final, out.negative_rc_edges);
        ++out.pricing_calls;
        out.pricing_s += secs(tp);
    }
    if (std::isfinite(out.global_lb)) out.global_lb_int = static_cast<int64_t>(std::ceil(out.global_lb - 1e-6));
    out.total_s = secs(t0);
    return out;
}

RootLoopResult run_root_loop(const Instance& inst, CandidateGraph& g, const RootLoopConfig& cfg, TelemetrySink* tel) {
    const auto t0 = Clock::now();
    CutLoop loop(inst, g, cfg, tel);
    NodeLimits lim;
    lim.max_rounds = cfg.max_rounds;
    lim.tailing_window = cfg.tailing_window;
    lim.tailing_rel = cfg.tailing_rel;
    lim.time_limit_s = cfg.time_limit_s;
    auto out = loop.solve({}, nullptr, lim);
    if (cfg.ub >= 0 && std::isfinite(out.global_lb))
        apply_root_rcf(g, out.final, out.global_lb, cfg.ub, out.rcf_fixed0, out.rcf_fixed1);
    out.lp = loop.lp();
    for (uint32_t j = 0; j < out.lp.upper.size(); ++j) {  // reflect the RCF fixes in the returned LP
        if (g.edges[j].status & kFixed0) out.lp.upper[j] = 0.0;
        if (g.edges[j].status & kFixed1) out.lp.lower[j] = 1.0;
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
