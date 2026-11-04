#include "cuts/mincut.hpp"

#include <algorithm>
#include <limits>
#include <queue>
#include <random>
#include <set>
#include <utility>

namespace apex {

// ---------------------------------------------------------------- Stoer-Wagner

StoerWagner::StoerWagner(const SupportGraph& g) : n_(g.n), adj_(g.n) {
    for (const auto& e : g.edges) {
        if (e.u == e.v) continue;
        adj_[e.u].push_back({e.v, e.cap});
        adj_[e.v].push_back({e.u, e.cap});
    }
}

CutResult StoerWagner::run(double phase_threshold) {
    phase_cuts_.clear();
    CutResult best;
    best.value = std::numeric_limits<double>::infinity();
    if (n_ < 2) { best.value = 0; return best; }

    std::vector<uint32_t> rep(n_);  // representative (merged-into) node
    for (uint32_t i = 0; i < n_; ++i) rep[i] = i;
    auto find = [&](uint32_t v) {
        while (rep[v] != v) v = rep[v] = rep[rep[v]];
        return v;
    };
    std::vector<std::vector<uint32_t>> members(n_);
    for (uint32_t i = 0; i < n_; ++i) members[i] = {i};
    std::vector<uint32_t> alive(n_);
    for (uint32_t i = 0; i < n_; ++i) alive[i] = i;

    std::vector<double> key(n_);
    std::vector<uint8_t> added(n_);
    while (alive.size() > 1) {
        for (uint32_t v : alive) { key[v] = 0; added[v] = 0; }
        std::priority_queue<std::pair<double, uint32_t>> pq;  // lazy: stale entries skipped
        for (uint32_t v : alive) pq.push({0.0, v});
        uint32_t prev = alive[0], last = alive[0];
        double last_key = 0;
        for (size_t step = 0; step < alive.size(); ++step) {
            uint32_t v;
            double k;
            for (;;) {
                auto [kk, vv] = pq.top();
                pq.pop();
                if (!added[vv] && kk == key[vv]) { v = vv; k = kk; break; }
            }
            added[v] = 1;
            prev = last;
            last = v;
            last_key = k;
            for (const auto& [w0, c] : adj_[v]) {
                const uint32_t w = find(w0);
                if (w == v || added[w]) continue;
                key[w] += c;
                pq.push({key[w], w});
            }
        }
        // Cut of the phase: {last} vs the rest, value = key of last.
        if (last_key < best.value) {
            best.value = last_key;
            best.side = members[last];
        }
        if (last_key < phase_threshold) phase_cuts_.push_back({last_key, members[last]});
        // Merge last into prev.
        rep[last] = prev;
        members[prev].insert(members[prev].end(), members[last].begin(), members[last].end());
        members[last].clear();
        adj_[prev].insert(adj_[prev].end(), adj_[last].begin(), adj_[last].end());
        adj_[last].clear();
        alive.erase(std::find(alive.begin(), alive.end(), last));
    }
    std::sort(best.side.begin(), best.side.end());
    for (auto& c : phase_cuts_) std::sort(c.side.begin(), c.side.end());
    return best;
}

// ---------------------------------------------------------------- Dinic

Dinic::Dinic(const SupportGraph& g) : n_(g.n), head_(g.n, -1) {
    for (const auto& e : g.edges)
        if (e.u != e.v) add_arc(e.u, e.v, e.cap, e.cap);
}

void Dinic::add_arc(uint32_t u, uint32_t v, double cap, double rev_cap) {
    arcs_.push_back({v, head_[u], cap, 0.0});
    head_[u] = static_cast<int32_t>(arcs_.size() - 1);
    arcs_.push_back({u, head_[v], rev_cap, 0.0});
    head_[v] = static_cast<int32_t>(arcs_.size() - 1);
}

bool Dinic::bfs(uint32_t s, uint32_t t) {
    level_.assign(n_, -1);
    std::vector<uint32_t> q{s};
    level_[s] = 0;
    for (size_t k = 0; k < q.size(); ++k) {
        const uint32_t v = q[k];
        for (int32_t a = head_[v]; a >= 0; a = arcs_[a].next) {
            const auto& arc = arcs_[a];
            if (level_[arc.to] < 0 && arc.cap - arc.flow > 1e-12) {
                level_[arc.to] = level_[v] + 1;
                q.push_back(arc.to);
            }
        }
    }
    return level_[t] >= 0;
}

// ponytail: recursive augmenting DFS (depth <= BFS level of t); fine on shrunken graphs, make it
// iterative before running pairwise cuts on unshrunken 100k-node supports.
double Dinic::dfs(uint32_t v, uint32_t t, double f) {
    if (v == t) return f;
    for (int32_t& a = it_[v]; a >= 0; a = arcs_[a].next) {
        auto& arc = arcs_[a];
        if (level_[arc.to] != level_[v] + 1 || arc.cap - arc.flow <= 1e-12) continue;
        const double pushed = dfs(arc.to, t, std::min(f, arc.cap - arc.flow));
        if (pushed > 0) {
            arc.flow += pushed;
            arcs_[a ^ 1].flow -= pushed;
            return pushed;
        }
    }
    return 0;
}

double Dinic::max_flow(uint32_t s, uint32_t t) {
    double flow = 0;
    if (s == t) return 0;
    while (bfs(s, t)) {
        it_ = head_;
        while (const double f = dfs(s, t, std::numeric_limits<double>::infinity())) flow += f;
    }
    return flow;
}

CutResult Dinic::min_cut_between(uint32_t s, uint32_t t) {
    for (auto& a : arcs_) a.flow = 0;
    CutResult r;
    r.value = max_flow(s, t);
    bfs(s, t);  // residual reachability from s
    for (uint32_t v = 0; v < n_; ++v)
        if (level_[v] >= 0) r.side.push_back(v);
    return r;
}

// ---------------------------------------------------------------- SEC separation

namespace {

// Canonical SEC side (S and its complement define the same SEC): fewer nodes; on ties, the side whose
// smallest node is smaller.
std::vector<uint32_t> canonical(std::vector<uint32_t> s, uint32_t n) {
    std::sort(s.begin(), s.end());
    if (2 * s.size() < n) return s;
    std::vector<uint32_t> comp;
    comp.reserve(n - s.size());
    size_t k = 0;
    for (uint32_t v = 0; v < n; ++v) {
        if (k < s.size() && s[k] == v) { ++k; continue; }
        comp.push_back(v);
    }
    if (comp.size() < s.size()) return comp;
    if (comp.size() == s.size() && !comp.empty() && comp[0] < s[0]) return comp;
    return s;
}

}  // namespace

std::vector<SecCut> separate_sec(const SupportGraph& g, const SecSeparatorConfig& cfg, SecSeparationStats* stats) {
    SecSeparationStats st;
    std::set<std::vector<uint32_t>> seen;
    std::vector<SecCut> out;
    const double limit = 2.0 - cfg.violation_tol;
    auto add = [&](const std::vector<uint32_t>& nodes, uint32_t& counter) {
        if (nodes.empty() || nodes.size() >= g.n) return;
        auto c = canonical(nodes, g.n);
        const double v = cut_value(g, c);  // exact re-verification on the original support
        if (v >= limit) return;
        if (!seen.insert(c).second) return;
        out.push_back({std::move(c), 2.0 - v});
        ++counter;
    };

    // 1. Components.
    const auto comps = connected_components(g);
    if (comps.size() > 1) {
        for (const auto& c : comps) add(c, st.component_cuts);
    } else {
        // 2. Shrink + Stoer-Wagner (global min cut + violated phase cuts).
        const auto sh = shrink_unit_edges(g);
        st.shrunk_nodes = sh.g.n;
        for (uint32_t k = 0; k < sh.g.n; ++k)  // degree-<2 supernodes are violated SECs by themselves
            if (cut_value(sh.g, {k}) < limit) add(sh.expand({k}), st.stoer_wagner_cuts);
        if (sh.g.n >= 2) {
            StoerWagner sw(sh.g);
            const auto best = sw.run(limit);
            if (best.value < limit) add(sh.expand(best.side), st.stoer_wagner_cuts);
            for (const auto& pc : sw.phase_cuts()) add(sh.expand(pc.side), st.stoer_wagner_cuts);

            // 3. Guided pairwise min cuts between supernodes touching fractional edges.
            std::vector<uint32_t> frac;
            std::vector<uint8_t> is_frac(sh.g.n, 0);
            for (const auto& e : sh.g.edges)
                if (e.cap < 1.0 - 1e-9) {
                    if (!is_frac[e.u]) { is_frac[e.u] = 1; frac.push_back(e.u); }
                    if (!is_frac[e.v]) { is_frac[e.v] = 1; frac.push_back(e.v); }
                }
            if (frac.size() >= 2 && cfg.max_pairs_per_round > 0) {
                std::mt19937_64 rng(cfg.seed);
                Dinic dinic(sh.g);
                for (uint32_t p = 0; p < cfg.max_pairs_per_round; ++p) {
                    const uint32_t s = frac[rng() % frac.size()], t = frac[rng() % frac.size()];
                    if (s == t) continue;
                    const auto cut = dinic.min_cut_between(s, t);
                    if (cut.value < limit) add(sh.expand(cut.side), st.pairwise_cuts);
                }
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const SecCut& a, const SecCut& b) { return a.violation > b.violation; });
    if (stats) *stats = st;
    return out;
}

}  // namespace apex
