#include "bnc/tree.hpp"

#include "bnc/rcf.hpp"
#include "bnc/checkpoint.hpp"
#include "core/serial.hpp"
#include "core/tour_io.hpp"
#include "primal/lk_lite.hpp"
#include "primal/lp_tour.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>

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
        case BnCResult::Status::Optimal: return "OPTIMAL_PROVEN";
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

namespace {

struct Offer {
    Tour tour;
    const char* source;
};

struct Observation {  // pseudocost observation in the producing worker's edge indexing
    uint32_t edge;
    uint8_t dir;
    double gain;
};

// Everything a processed node hands back to the master; no shared state is touched while a node
// is processed, so workers can run in parallel and the merge order alone fixes the result.
struct NodeOutcome {
    enum class Kind { Pruned, Infeasible, Integral, Reopen, Branched } kind = Kind::Pruned;
    std::vector<Offer> offers;
    std::vector<BnCNode> children;  // Reopen: the node itself; Branched: two children (ids unset)
    std::vector<Observation> obs;
    bool found_integral = false, warm = false, warm_dual_bad = false;
    double lp_s = 0, sep_s = 0, pricing_s = 0, heuristic_s = 0;
    uint64_t pricing_calls = 0, priced = 0, heuristic_calls = 0, strong_pivots = 0, strong_candidates = 0, rcf_node_fixings = 0;
};

// Node outcome after its cut-and-price loop. A tour whose Farley bound is still below the UB was
// interrupted (time limit) before pricing finished: the node is reopened with its valid bound.
void conclude(const Instance& inst, CandidateGraph& g, CutLoop& loop, Pseudocosts& pc, const BnCConfig& cfg,
              const RootLoopResult& r, BnCNode& node, int64_t ub, NodeOutcome& o) {
    o.lp_s += r.lp_s;
    o.sep_s += r.sep_s;
    o.pricing_s += r.pricing_s;
    o.pricing_calls += r.pricing_calls;
    o.priced += r.priced_edges;
    if (r.infeasible) { o.kind = NodeOutcome::Kind::Infeasible; return; }
    if (r.cutoff) { o.kind = NodeOutcome::Kind::Pruned; return; }
    bool tour = false;
    if (integral(r.final.x)) {
        auto t = extract_tour(inst, g, r.final.x);
        if (!t.order.empty()) {
            tour = true;
            o.found_integral = true;
            if (t.cost < ub) ub = t.cost;
            o.offers.push_back({std::move(t), "lp_integral"});
        }
    } else if (cfg.lp_heuristic) {  // heuristic (UB only): greedy by x + 2-opt/Or-opt
        const auto th = Clock::now();
        auto t = lp_guided_tour(inst, g, r.final.x);
        o.heuristic_s += secs(th);
        ++o.heuristic_calls;
        if (is_valid_tour(t, inst.n)) {
            t.cost = tour_cost(inst, t);
            if (t.cost < ub) ub = t.cost;
            o.offers.push_back({std::move(t), "lp_guided"});
        }
    }
    if (prunable(r.global_lb, ub)) { o.kind = tour ? NodeOutcome::Kind::Integral : NodeOutcome::Kind::Pruned; return; }
    if (tour || !std::isfinite(r.global_lb)) {
        node.dual_bound = std::max(node.dual_bound, r.global_lb);
        o.kind = NodeOutcome::Kind::Reopen;
        o.children.push_back(std::move(node));
        return;
    }
    const auto d = select_branch(loop.lp_mut(), loop.basis().basis, r.final, cfg.root.lp, pc, cfg.branching);
    const Lp& lp = loop.lp();  // bounds restored by select_branch
    o.strong_pivots += d.strong_pivots;
    o.strong_candidates += d.strong_candidates;
    if (d.edge < 0) throw std::logic_error("bnc: fractional node without a branching candidate");
    const auto e = static_cast<uint32_t>(d.edge);
    auto basis = std::make_shared<const NodeBasis>(loop.basis());
    // Node-scoped RCF with this node's Farley bound: valid in its subtree only, so the fixes
    // travel in the children's bound changes (MASTER §5.5 scoping).
    BoundChanges fixes;
    for (const auto& f : rcf_fixings(r.final, r.global_lb, ub)) {
        if (f.edge == e || lp.lower[f.edge] == lp.upper[f.edge] || (g.edges[f.edge].status & (kFixed0 | kFixed1))) continue;
        fixes.push_back(f);
    }
    o.rcf_node_fixings += fixes.size();
    for (uint8_t v : {uint8_t{0}, uint8_t{1}}) {  // x_e = 1 pushed last: the dive takes it first
        BnCNode c{0, node.depth + 1, r.global_lb, node.bounds, basis, NodeState::Open,
                  d.edge, v, v ? 1 - r.final.x[e] : r.final.x[e], r.lp_value};
        c.bounds.insert(c.bounds.end(), fixes.begin(), fixes.end());
        c.bounds.push_back({e, v});
        o.children.push_back(std::move(c));
    }
    o.kind = NodeOutcome::Kind::Branched;
}

NodeOutcome process_node(const Instance& inst, CandidateGraph& g, CutLoop& loop, Pseudocosts& pc, const BnCConfig& cfg,
                         BnCNode node, int64_t ub, double time_left) {
    NodeOutcome o;
    NodeLimits lim = cfg.node;
    lim.cutoff = ub;
    lim.time_limit_s = std::max(0.0, time_left);
    const bool warm_check = node.parent_basis && loop.same_columns(*node.parent_basis);
    const auto r = loop.solve(node.bounds, node.parent_basis.get(), lim);
    // Bound changes alone never break dual feasibility of the parent basis (MASTER M3.1).
    o.warm_dual_bad = warm_check && r.first_dual_flips > 0;
    o.warm = node.parent_basis != nullptr;
    if (node.branch_edge >= 0 && !r.infeasible && (r.converged || r.tailing_off)) {  // full child LP gain
        const double gain = std::max(0.0, r.lp_value - node.parent_lp) / node.branch_frac;
        pc.observe(static_cast<uint32_t>(node.branch_edge), node.branch_dir, gain);
        o.obs.push_back({static_cast<uint32_t>(node.branch_edge), node.branch_dir, gain});
    }
    conclude(inst, g, loop, pc, cfg, r, node, ub, o);
    return o;
}

// ---- Checkpoint v2 -------------------------------------------------------------------------

uint64_t fnv64(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

void save_graph(std::ostream& o, const CandidateGraph& g) {
    using namespace serial;
    tag(o, "graph");
    put(o, g.n);
    put(o, g.fallback_edges);
    put(o, static_cast<uint64_t>(g.edges.size()));
    o << '\n';
    for (const auto& e : g.edges) {
        put(o, e.u); put(o, e.v); put(o, e.cost); put(o, e.status); put(o, e.reduced_cost);
        o << '\n';
    }
}

void load_graph(std::istream& in, CandidateGraph& g) {
    using namespace serial;
    expect(in, "graph");
    g.n = get<uint32_t>(in);
    g.fallback_edges = get<uint32_t>(in);
    const auto m = get<uint64_t>(in);
    g.edges.clear();
    for (uint64_t k = 0; k < m; ++k) {
        Edge e{};
        e.u = get<uint32_t>(in); e.v = get<uint32_t>(in); e.cost = get<int32_t>(in);
        e.status = get<uint32_t>(in); e.reduced_cost = get<double>(in);
        g.edges.push_back(e);
    }
    rebuild_adjacency(g);
}

void save_basis(std::ostream& o, const NodeBasis& b) {
    using namespace serial;
    std::vector<uint8_t> col, row;
    for (auto s : b.basis.col) col.push_back(static_cast<uint8_t>(s));
    for (auto s : b.basis.row) row.push_back(static_cast<uint8_t>(s));
    put_vec(o, col);
    put_vec(o, row);
    put_vec(o, std::vector<uint64_t>(b.active.begin(), b.active.end()));
}

NodeBasis load_basis(std::istream& in) {
    using namespace serial;
    NodeBasis b;
    for (auto s : get_vec<uint8_t>(in)) b.basis.col.push_back(static_cast<VarState>(s));
    for (auto s : get_vec<uint8_t>(in)) b.basis.row.push_back(static_cast<VarState>(s));
    const auto a = get_vec<uint64_t>(in);
    b.active.assign(a.begin(), a.end());
    return b;
}

}  // namespace

// Master state of one branch-and-cut run.
class Tree {
public:
    Tree(const Instance& inst, CandidateGraph& g, const BnCConfig& cfg, TelemetrySink* tel)
        : inst_(inst), g_(g), cfg_(cfg), tel_(tel), loop_(inst, g, cfg.root, tel) {
        if (cfg.parallel_lps < 1 || cfg.parallel_lps > 2)
            throw std::invalid_argument("--parallel-lps must be 1 or 2 in Sprint 3 (memory-aware scheduler: Sprint 4)");
    }

    BnCResult run(Tour initial) {
        t0_ = Clock::now();
        if (cfg_.resume) {
            load();
        } else {
            out_.best = std::move(initial);
            out_.best.cost = tour_cost(inst_, out_.best);
            out_.ub = out_.best.cost;
            if (!cfg_.tour_path.empty()) write_tour_file(cfg_.tour_path, inst_, out_.best, "apex_tsp initial ub " + std::to_string(out_.ub));
            root();
        }
        last_ckpt_ = Clock::now();
        bool timed_out = false;
        while (!open_.empty()) {
            if (elapsed() > cfg_.time_limit_s) { timed_out = true; break; }
            if (out_.nodes >= cfg_.max_nodes) break;
            poll_primal();
            if (rcf_pending_) root_rcf();
            if (cfg_.parallel_lps > 1) batch(); else serial_step();
            if (!cfg_.checkpoint_dir.empty() && secs(last_ckpt_) >= cfg_.checkpoint_every_s) save();
        }
        if (!open_.empty() && elapsed() > cfg_.time_limit_s) timed_out = true;
        if (!open_.empty() && !cfg_.checkpoint_dir.empty()) save();  // stopped early: resumable
        out_.lb = static_cast<double>(out_.ub);
        for (const auto& nd : open_) out_.lb = std::min(out_.lb, nd.dual_bound);
        out_.lb_int = static_cast<int64_t>(std::ceil(out_.lb - 1e-6));
        out_.status = open_.empty() ? BnCResult::Status::Optimal
                                    : (timed_out ? BnCResult::Status::TimeLimit : BnCResult::Status::NodeLimit);
        out_.elapsed_s = elapsed();
        out_.total_s = secs(t0_);
        if (tel_)
            tel_->event("FINISHED", {{"status", status_name(out_.status)}, {"ub", std::to_string(out_.ub)}, {"lb", fmt(out_.lb)},
                                     {"gap_pct", fmt(100.0 * (out_.ub - out_.lb) / std::max(1.0, static_cast<double>(out_.ub)))},
                                     {"nodes", std::to_string(out_.nodes)}, {"open", std::to_string(open_.size())},
                                     {"max_depth", std::to_string(out_.max_depth)}, {"root_lb", fmt(out_.root_lb)},
                                     {"verification_pricing_calls", std::to_string(out_.verification_pricing_calls)},
                                     {"priced_columns", std::to_string(out_.priced_columns)},
                                     {"certificate", out_.status == BnCResult::Status::Optimal && out_.lb_int == out_.ub &&
                                                             tour_cost(inst_, out_.best) == out_.ub ? "ok" : "none"},
                                     {"parallel_lps", std::to_string(cfg_.parallel_lps)},
                                     {"open_bytes", std::to_string(open_bytes())}, {"pool_cuts", std::to_string(loop_.pool().size())},
                                     {"edges", std::to_string(g_.edges.size())},
                                     {"checkpoints", std::to_string(out_.checkpoints_written)},
                                     {"lp_s", fmt(out_.lp_s, 2)}, {"sep_s", fmt(out_.sep_s, 2)},
                                     {"pricing_s", fmt(out_.pricing_s, 2)}, {"total_s", fmt(out_.total_s, 2)}});
        return std::move(out_);
    }

private:
    double elapsed() const { return elapsed0_ + secs(t0_); }

    // Memory held by the open frontier: bound-change lists plus distinct parent bases.
    uint64_t open_bytes() const {
        uint64_t b = 0;
        std::map<const NodeBasis*, int> seen;
        for (const auto& nd : open_) {
            b += nd.bounds.size() * sizeof(BoundChange);
            if (nd.parent_basis && seen.emplace(nd.parent_basis.get(), 0).second)
                b += nd.parent_basis->basis.col.size() + nd.parent_basis->basis.row.size() + 8 * nd.parent_basis->active.size();
        }
        return b;
    }

    // Root: cut-and-price with the root limits; its outcome is applied like any node's, then the
    // permanent root RCF runs against the (possibly improved) UB.
    void root() {
        NodeLimits lim;
        lim.max_rounds = cfg_.root.max_rounds;
        lim.tailing_window = cfg_.root.tailing_window;
        lim.tailing_rel = cfg_.root.tailing_rel;
        lim.time_limit_s = cfg_.time_limit_s;
        out_.root = loop_.solve({}, nullptr, lim);
        if (out_.root.infeasible) throw std::logic_error("bnc: root LP infeasible over the complete graph");
        out_.root_lb = out_.root.global_lb;
        out_.nodes = 1;
        out_.processing_order.push_back(0);
        BnCNode node{0, 0, std::isfinite(out_.root_lb) ? out_.root_lb : -INFINITY, {}, nullptr, NodeState::Processing};
        NodeOutcome o;
        conclude(inst_, g_, loop_, pc_, cfg_, out_.root, node, out_.ub, o);
        apply(o, false);
        apply_root_rcf(g_, out_.root.final, out_.root_lb, out_.ub, out_.root.rcf_fixed0, out_.root.rcf_fixed1);
        rcf_pending_ = false;
    }

    void root_rcf() {  // root reduced costs + root bound stay valid globally; a new UB fixes more
        rcf_pending_ = false;
        const uint32_t f0 = out_.root.rcf_fixed0, f1 = out_.root.rcf_fixed1;
        apply_root_rcf(g_, out_.root.final, out_.root_lb, out_.ub, out_.root.rcf_fixed0, out_.root.rcf_fixed1);
        if (tel_)
            tel_->event("rcf_root", {{"ub", std::to_string(out_.ub)}, {"new_fixed0", std::to_string(out_.root.rcf_fixed0 - f0)},
                                     {"new_fixed1", std::to_string(out_.root.rcf_fixed1 - f1)},
                                     {"pruned_edges_rcf", std::to_string(out_.root.rcf_fixed0)}});
    }

    // Next node by the selection rule, skipping (and counting) nodes the UB already prunes.
    bool pop(BnCNode& node) {
        while (!open_.empty()) {
            size_t pick = open_.size() - 1;  // dive: last pushed
            if (found_integral_ || dive_over_ || cfg_.select == BnCConfig::Select::BestBound) {
                for (size_t k = 0; k < open_.size(); ++k)  // ponytail: O(open) scan; heap if fronts grow past ~1e5
                    if (open_[k].dual_bound < open_[pick].dual_bound ||
                        (open_[k].dual_bound == open_[pick].dual_bound && open_[k].id < open_[pick].id))
                        pick = k;
            }
            node = std::move(open_[pick]);
            open_.erase(open_.begin() + static_cast<std::ptrdiff_t>(pick));
            if (prunable(node.dual_bound, out_.ub)) { ++out_.pruned; continue; }
            ++out_.nodes;
            out_.max_depth = std::max<uint64_t>(out_.max_depth, node.depth);
            if (node.depth >= cfg_.dive_depth_limit) dive_over_ = true;  // dive went too deep: best-bound from now on
            out_.processing_order.push_back(node.id);
            return true;
        }
        return false;
    }

    void serial_step() {
        BnCNode node;
        if (!pop(node)) return;
        auto o = process_node(inst_, g_, loop_, pc_, cfg_, std::move(node), out_.ub, cfg_.time_limit_s - elapsed());
        apply(o, false);
        progress();
    }

    // Deterministic parallel batch: up to parallel_lps nodes, each on a snapshot (graph, cut loop,
    // pseudocosts) of the master state; outcomes merged in worker order. Throughput cost: the batch
    // waits for its slowest node, and work found by one worker reaches the others only at merge.
    void batch() {
        std::vector<BnCNode> nodes;
        for (uint32_t w = 0; w < cfg_.parallel_lps; ++w) {
            BnCNode nd;
            if (!pop(nd)) break;
            nodes.push_back(std::move(nd));
        }
        if (nodes.empty()) return;
        ++out_.batches;
        const size_t W = nodes.size(), m0 = g_.edges.size(), p0 = loop_.pool().size();
        std::vector<CandidateGraph> gs(W, g_);
        std::vector<std::unique_ptr<CutLoop>> loops;
        std::vector<Pseudocosts> pcs(W, pc_);
        for (size_t w = 0; w < W; ++w) loops.push_back(std::make_unique<CutLoop>(loop_, gs[w]));
        std::vector<NodeOutcome> res(W);
        std::vector<std::exception_ptr> err(W);
        const int64_t ub = out_.ub;
        const double left = cfg_.time_limit_s - elapsed();
        std::vector<std::thread> th;
        for (size_t w = 0; w < W; ++w)
            th.emplace_back([&, w] {
                try {
                    res[w] = process_node(inst_, gs[w], *loops[w], pcs[w], cfg_, std::move(nodes[w]), ub, left);
                } catch (...) {
                    err[w] = std::current_exception();
                }
            });
        for (auto& t : th) t.join();
        for (auto& e : err)
            if (e) std::rethrow_exception(e);
        for (size_t w = 0; w < W; ++w) {
            merge(res[w], gs[w], *loops[w], m0, p0);
            apply(res[w], true);
        }
        loop_.reset_to(last_worker_basis_);  // rows found by the batch carry forward, as in the serial loop

        if (peak_rss_bytes() > cfg_.ram_cap_bytes)
            throw std::runtime_error("bnc: peak RSS above the parallel-lps memory cap (" + std::to_string(cfg_.ram_cap_bytes) + " bytes)");
        progress();
    }

    // Maps a worker outcome onto master indices: edges and cuts the worker added (beyond the
    // snapshot sizes m0 / p0) are appended to (or found in) the master graph and pool.
    void merge(NodeOutcome& o, const CandidateGraph& wg, CutLoop& wl, size_t m0, size_t p0) {
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> index;
        for (uint32_t j = static_cast<uint32_t>(m0); j < g_.edges.size(); ++j) index[{g_.edges[j].u, g_.edges[j].v}] = j;
        std::vector<uint32_t> col(wg.edges.size());
        bool grew = false;
        for (uint32_t j = 0; j < wg.edges.size(); ++j) {
            if (j < m0) { col[j] = j; continue; }
            const auto key = std::make_pair(wg.edges[j].u, wg.edges[j].v);
            auto it = index.find(key);
            if (it == index.end()) {
                it = index.emplace(key, static_cast<uint32_t>(g_.edges.size())).first;
                g_.edges.push_back(wg.edges[j]);
                grew = true;
            }
            col[j] = it->second;
        }
        if (grew) rebuild_adjacency(g_);
        std::vector<size_t> cut(wl.pool().size());
        for (size_t k = 0; k < cut.size(); ++k) cut[k] = k < p0 ? k : loop_.pool_mut().add_or_find(wl.pool().at(k));
        auto map_basis = [&](const NodeBasis& b) {
            NodeBasis nb;
            nb.basis.col.assign(g_.edges.size(), VarState::AtLower);
            for (size_t j = 0; j < b.basis.col.size(); ++j) nb.basis.col[col[j]] = b.basis.col[j];
            nb.basis.row = b.basis.row;
            for (size_t k : b.active) nb.active.push_back(cut[k]);
            return nb;
        };
        std::map<const NodeBasis*, std::shared_ptr<const NodeBasis>> done;
        for (auto& c : o.children) {
            for (auto& b : c.bounds) b.edge = col[b.edge];
            if (c.branch_edge >= 0) c.branch_edge = col[static_cast<size_t>(c.branch_edge)];
            if (!c.parent_basis) continue;
            auto& mapped = done[c.parent_basis.get()];
            if (!mapped) mapped = std::make_shared<const NodeBasis>(map_basis(*c.parent_basis));
            c.parent_basis = mapped;
        }
        for (auto& ob : o.obs) ob.edge = col[ob.edge];
        last_worker_basis_ = map_basis(wl.basis());
    }

    // Applies an outcome to the master state; worker outcomes (merged) also carry pseudocost
    // observations, serial ones already updated pc_ in place.
    void apply(NodeOutcome& o, bool observations) {
        for (auto& of : o.offers) offer(std::move(of.tour), of.source);
        found_integral_ = found_integral_ || o.found_integral;
        out_.lp_s += o.lp_s;
        out_.sep_s += o.sep_s;
        out_.pricing_s += o.pricing_s;
        out_.heuristic_s += o.heuristic_s;
        out_.verification_pricing_calls += o.pricing_calls;
        out_.priced_columns += o.priced;
        out_.heuristic_calls += o.heuristic_calls;
        out_.strong_pivots += o.strong_pivots;
        out_.strong_candidates += o.strong_candidates;
        out_.max_node_strong_pivots = std::max(out_.max_node_strong_pivots, o.strong_pivots);
        out_.rcf_node_fixings += o.rcf_node_fixings;
        if (o.warm_dual_bad) ++out_.warm_dual_infeasible;
        if (out_.processing_order.size() > 1) ++(o.warm ? out_.warm_starts : out_.cold_starts);
        if (observations)
            for (const auto& ob : o.obs) pc_.observe(ob.edge, ob.dir, ob.gain);
        switch (o.kind) {
            case NodeOutcome::Kind::Pruned: ++out_.pruned; break;
            case NodeOutcome::Kind::Infeasible: ++out_.infeasible; break;
            case NodeOutcome::Kind::Integral: ++out_.integral; break;
            case NodeOutcome::Kind::Reopen: open_.push_back(std::move(o.children[0])); break;
            case NodeOutcome::Kind::Branched:
                for (auto& c : o.children) {
                    c.id = next_id_++;
                    open_.push_back(std::move(c));
                }
                ++out_.branched;
                break;
        }
    }

    // Every UB candidate goes through here: cost recomputed with the metric (never the LP sum),
    // the .tour file rewritten atomically on each improvement.
    void offer(Tour t, const char* source) {
        if (!is_valid_tour(t, inst_.n)) return;
        t.cost = tour_cost(inst_, t);
        if (t.cost >= out_.ub) return;
        out_.ub = t.cost;
        out_.best = std::move(t);
        ++out_.ub_improvements;
        rcf_pending_ = true;
        if (!cfg_.tour_path.empty())
            write_tour_file(cfg_.tour_path, inst_, out_.best, "apex_tsp " + std::string(source) + " ub " + std::to_string(out_.ub));
        if (tel_) tel_->event("ub_improved", {{"nodes", std::to_string(out_.nodes)}, {"ub", std::to_string(out_.ub)}, {"source", source}});
    }

    void poll_primal() {
        Tour t;
        if (cfg_.primal && cfg_.primal->take_if_better(out_.ub, t)) offer(std::move(t), "ils");
    }

    void progress() {
        if (tel_ && out_.nodes / 1000 != last_progress_) {
            last_progress_ = out_.nodes / 1000;
            double lb = static_cast<double>(out_.ub);
            for (const auto& nd : open_) lb = std::min(lb, nd.dual_bound);
            tel_->event("tree_progress", {{"nodes", std::to_string(out_.nodes)}, {"open", std::to_string(open_.size())},
                                          {"ub", std::to_string(out_.ub)}, {"lb", fmt(lb)}});
        }
    }

    // ---- checkpoint v2: <dir>/v2-<seq>/{state,graph,loop,pseudocosts,nodes}.txt + manifest.txt,
    // then <dir>/LATEST (atomic rename) names the snapshot; older snapshots are removed.
    void save() {
        namespace fs = std::filesystem;
        using namespace serial;
        last_ckpt_ = Clock::now();
        const fs::path dir(cfg_.checkpoint_dir);
        fs::create_directories(dir);
        const std::string name = "v2-" + std::to_string(out_.checkpoints_written + 1);
        std::map<std::string, std::string> files;
        {
            std::ostringstream o;
            tag(o, "state");
            put(o, elapsed());
            put(o, next_id_); put(o, found_integral_); put(o, dive_over_); put(o, rcf_pending_);
            put(o, out_.ub); put(o, out_.root_lb);
            put(o, out_.root.rcf_fixed0); put(o, out_.root.rcf_fixed1);
            o << '\n';
            for (uint64_t v : {out_.nodes, out_.pruned, out_.infeasible, out_.integral, out_.branched, out_.max_depth,
                               out_.ub_improvements, out_.warm_starts, out_.cold_starts, out_.warm_dual_infeasible,
                               out_.verification_pricing_calls, out_.priced_columns, out_.rcf_node_fixings, out_.strong_pivots,
                               out_.strong_candidates, out_.max_node_strong_pivots, out_.heuristic_calls,
                               out_.checkpoints_written + 1, out_.batches})
                put(o, v);
            for (double v : {out_.heuristic_s, out_.lp_s, out_.sep_s, out_.pricing_s}) put(o, v);
            o << '\n';
            put_vec(o, out_.best.order);
            put_vec(o, out_.processing_order);
            put_vec(o, out_.root.final.var_state);
            put_vec(o, out_.root.final.reduced_costs);
            files["state.txt"] = o.str();
        }
        { std::ostringstream o; save_graph(o, g_); files["graph.txt"] = o.str(); }
        { std::ostringstream o; loop_.save(o); files["loop.txt"] = o.str(); }
        { std::ostringstream o; pc_.save(o); files["pseudocosts.txt"] = o.str(); }
        {
            std::ostringstream o;
            tag(o, "nodes");
            std::map<const NodeBasis*, uint64_t> ids;
            std::vector<const NodeBasis*> bases;
            for (const auto& nd : open_)
                if (nd.parent_basis && ids.emplace(nd.parent_basis.get(), bases.size()).second) bases.push_back(nd.parent_basis.get());
            put(o, static_cast<uint64_t>(bases.size()));
            o << '\n';
            for (const auto* b : bases) save_basis(o, *b);
            put(o, static_cast<uint64_t>(open_.size()));
            o << '\n';
            for (const auto& nd : open_) {
                put(o, nd.id); put(o, nd.depth); put(o, nd.dual_bound);
                put(o, nd.parent_basis ? static_cast<int64_t>(ids[nd.parent_basis.get()]) : int64_t{-1});
                put(o, nd.branch_edge); put(o, nd.branch_dir); put(o, nd.branch_frac); put(o, nd.parent_lp);
                put(o, static_cast<uint64_t>(nd.bounds.size()));
                for (const auto& b : nd.bounds) { put(o, b.edge); put(o, b.value); }
                o << '\n';
            }
            files["nodes.txt"] = o.str();
        }
        const fs::path tmp = dir / (name + ".tmp"), fin = dir / name;
        fs::remove_all(tmp);
        fs::create_directories(tmp);
        std::ostringstream man;
        man << "apex-bnc-checkpoint 2\ninstance_hash " << instance_hash(inst_) << "\nn " << inst_.n << "\nseed " << cfg_.seed << '\n';
        for (const auto& [f, data] : files) {
            std::ofstream os(tmp / f, std::ios::binary);
            os << data;
            if (!os.flush()) throw std::runtime_error((tmp / f).string() + ": write failed");
            man << "file " << f << ' ' << data.size() << ' ' << fnv64(data) << '\n';
        }
        {
            std::ofstream os(tmp / "manifest.txt", std::ios::binary);
            os << man.str() << "end\n";
            if (!os.flush()) throw std::runtime_error("checkpoint manifest: write failed");
        }
        fs::remove_all(fin);
        fs::rename(tmp, fin);
        {
            std::ofstream os(dir / "LATEST.tmp");
            os << name << '\n';
            if (!os.flush()) throw std::runtime_error("checkpoint LATEST: write failed");
        }
        fs::rename(dir / "LATEST.tmp", dir / "LATEST");  // the switch to the new snapshot is atomic
        for (const auto& entry : fs::directory_iterator(dir)) {
            const auto fn = entry.path().filename().string();
            if (fn.rfind("v2-", 0) == 0 && fn != name) fs::remove_all(entry.path());
        }
        ++out_.checkpoints_written;
        if (tel_) tel_->event("checkpoint", {{"version", "2"}, {"snapshot", name}, {"open", std::to_string(open_.size())},
                                             {"nodes", std::to_string(out_.nodes)}});
    }

    void load() {
        namespace fs = std::filesystem;
        using namespace serial;
        const fs::path dir(cfg_.checkpoint_dir);
        std::ifstream latest(dir / "LATEST");
        std::string name;
        if (!(latest >> name)) throw std::runtime_error("checkpoint: " + (dir / "LATEST").string() + " missing");
        std::ifstream mf(dir / name / "manifest.txt");
        std::string word;
        uint64_t version = 0, hash = 0, n = 0, seed = 0;
        if (!(mf >> word >> version) || word != "apex-bnc-checkpoint" || version != 2)
            throw std::runtime_error("checkpoint: not a v2 manifest");
        mf >> word >> hash >> word >> n >> word >> seed;
        if (hash != instance_hash(inst_) || n != inst_.n) throw std::runtime_error("checkpoint: instance mismatch");
        std::map<std::string, std::string> files;
        while (mf >> word && word == "file") {
            std::string f;
            uint64_t size = 0, h = 0;
            mf >> f >> size >> h;
            std::ifstream is(dir / name / f, std::ios::binary);
            std::string data((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
            if (data.size() != size || fnv64(data) != h) throw std::runtime_error("checkpoint: " + f + " corrupt (size/hash)");
            files[f] = std::move(data);
        }
        if (word != "end") throw std::runtime_error("checkpoint: manifest truncated");
        for (const char* f : {"state.txt", "graph.txt", "loop.txt", "pseudocosts.txt", "nodes.txt"})
            if (!files.count(f)) throw std::runtime_error(std::string("checkpoint: missing ") + f);
        { std::istringstream in(files["graph.txt"]); load_graph(in, g_); }
        { std::istringstream in(files["loop.txt"]); loop_.load(in); }
        { std::istringstream in(files["pseudocosts.txt"]); pc_.load(in); }
        {
            std::istringstream in(files["state.txt"]);
            expect(in, "state");
            elapsed0_ = get<double>(in);
            next_id_ = get<uint32_t>(in); found_integral_ = get<bool>(in); dive_over_ = get<bool>(in); rcf_pending_ = get<bool>(in);
            out_.ub = get<int64_t>(in); out_.root_lb = get<double>(in);
            out_.root.rcf_fixed0 = get<uint32_t>(in); out_.root.rcf_fixed1 = get<uint32_t>(in);
            for (uint64_t* v : {&out_.nodes, &out_.pruned, &out_.infeasible, &out_.integral, &out_.branched, &out_.max_depth,
                                &out_.ub_improvements, &out_.warm_starts, &out_.cold_starts, &out_.warm_dual_infeasible,
                                &out_.verification_pricing_calls, &out_.priced_columns, &out_.rcf_node_fixings, &out_.strong_pivots,
                                &out_.strong_candidates, &out_.max_node_strong_pivots, &out_.heuristic_calls,
                                &out_.checkpoints_written, &out_.batches})
                *v = get<uint64_t>(in);
            for (double* v : {&out_.heuristic_s, &out_.lp_s, &out_.sep_s, &out_.pricing_s}) *v = get<double>(in);
            out_.best.order = get_vec<uint32_t>(in);
            out_.processing_order = get_vec<uint32_t>(in);
            out_.root.final.var_state = get_vec<uint8_t>(in);
            out_.root.final.reduced_costs = get_vec<double>(in);
            out_.best.cost = tour_cost(inst_, out_.best);
            if (!is_valid_tour(out_.best, inst_.n) || out_.best.cost != out_.ub) throw std::runtime_error("checkpoint: best tour does not match UB");
        }
        {
            std::istringstream in(files["nodes.txt"]);
            expect(in, "nodes");
            std::vector<std::shared_ptr<const NodeBasis>> bases(get<uint64_t>(in));
            for (auto& b : bases) b = std::make_shared<const NodeBasis>(load_basis(in));
            const auto cnt = get<uint64_t>(in);
            open_.clear();
            for (uint64_t k = 0; k < cnt; ++k) {
                BnCNode nd{};
                nd.id = get<uint32_t>(in); nd.depth = get<uint32_t>(in); nd.dual_bound = get<double>(in);
                const auto bi = get<int64_t>(in);
                if (bi >= 0) nd.parent_basis = bases.at(static_cast<size_t>(bi));
                nd.branch_edge = get<int64_t>(in); nd.branch_dir = get<uint8_t>(in);
                nd.branch_frac = get<double>(in); nd.parent_lp = get<double>(in);
                const auto nb = get<uint64_t>(in);
                for (uint64_t i = 0; i < nb; ++i) {
                    const auto e = get<uint32_t>(in);
                    nd.bounds.push_back({e, get<uint8_t>(in)});
                }
                nd.state = NodeState::Open;
                open_.push_back(std::move(nd));
            }
        }
        if (tel_) tel_->event("resume", {{"snapshot", name}, {"open", std::to_string(open_.size())}, {"ub", std::to_string(out_.ub)}});
    }

    const Instance& inst_;
    CandidateGraph& g_;
    const BnCConfig& cfg_;
    TelemetrySink* tel_;
    CutLoop loop_;
    Pseudocosts pc_;
    BnCResult out_;
    std::vector<BnCNode> open_;
    uint32_t next_id_ = 1;
    bool found_integral_ = false, dive_over_ = false, rcf_pending_ = false;
    Clock::time_point t0_, last_ckpt_;
    double elapsed0_ = 0;
    uint64_t last_progress_ = 0;
    NodeBasis last_worker_basis_;
};

BnCResult run_bnc(const Instance& inst, CandidateGraph& g, const BnCConfig& cfg, Tour initial, TelemetrySink* tel) {
    Tree t(inst, g, cfg, tel);
    return t.run(std::move(initial));
}

}  // namespace apex
