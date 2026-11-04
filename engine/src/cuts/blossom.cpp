#include "cuts/blossom.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

#include "cuts/mincut.hpp"

namespace apex {
namespace {

std::vector<uint8_t> membership(uint32_t n, const std::vector<uint32_t>& s) {
    std::vector<uint8_t> in(n, 0);
    for (uint32_t v : s) in[v] = 1;
    return in;
}

// Canonical handle: H and V \ H give the same inequality; keep the smaller (ties: smaller first node).
std::vector<uint32_t> canonical_handle(std::vector<uint32_t> h, uint32_t n) {
    std::sort(h.begin(), h.end());
    h.erase(std::unique(h.begin(), h.end()), h.end());
    if (2 * h.size() < n) return h;
    std::vector<uint32_t> comp;
    size_t k = 0;
    for (uint32_t v = 0; v < n; ++v) {
        if (k < h.size() && h[k] == v) { ++k; continue; }
        comp.push_back(v);
    }
    if (comp.size() < h.size() || (comp.size() == h.size() && !comp.empty() && comp[0] < h[0])) return comp;
    return h;
}

}  // namespace

double blossom_violation(const SupportGraph& g, const std::vector<uint32_t>& handle,
                         const std::vector<BlossomTooth>& teeth) {
    const auto in = membership(g.n, handle);
    std::set<std::pair<uint32_t, uint32_t>> tset;
    for (const auto& t : teeth) tset.insert({t.u, t.v});
    double lhs = 0;
    for (const auto& e : g.edges) {
        if (in[e.u] == in[e.v]) continue;
        lhs += tset.count({std::min(e.u, e.v), std::max(e.u, e.v)}) ? -e.cap : e.cap;
    }
    return (1.0 - static_cast<double>(teeth.size())) - lhs;
}

double best_teeth(const SupportGraph& g, const std::vector<uint32_t>& handle, std::vector<BlossomTooth>& teeth) {
    teeth.clear();
    const auto in = membership(g.n, handle);
    // Per crossing edge: cost x as non-tooth, 1 - x as tooth. violation = 1 - sum(min cost).
    double sum = 0;
    int64_t worst_in = -1, best_out = -1;  // cheapest parity flip candidates
    double worst_in_delta = std::numeric_limits<double>::infinity(), best_out_delta = std::numeric_limits<double>::infinity();
    for (uint32_t k = 0; k < g.edges.size(); ++k) {
        const auto& e = g.edges[k];
        if (in[e.u] == in[e.v]) continue;
        const double as_tooth = 1.0 - e.cap, as_not = e.cap;
        if (as_tooth < as_not) {
            teeth.push_back({std::min(e.u, e.v), std::max(e.u, e.v), e.lp_col});
            sum += as_tooth;
            if (as_not - as_tooth < worst_in_delta) { worst_in_delta = as_not - as_tooth; worst_in = k; }
        } else {
            sum += as_not;
            if (as_tooth - as_not < best_out_delta) { best_out_delta = as_tooth - as_not; best_out = k; }
        }
    }
    if (teeth.size() % 2 == 0) {  // parity fix: cheapest of dropping a tooth or adding one
        if (worst_in < 0 && best_out < 0) { teeth.clear(); return -std::numeric_limits<double>::infinity(); }
        if (worst_in >= 0 && (best_out < 0 || worst_in_delta <= best_out_delta)) {
            const auto& e = g.edges[worst_in];
            const uint32_t a = std::min(e.u, e.v), b = std::max(e.u, e.v);
            teeth.erase(std::find_if(teeth.begin(), teeth.end(), [&](const BlossomTooth& t) { return t.u == a && t.v == b; }));
            sum += worst_in_delta;
        } else {
            const auto& e = g.edges[best_out];
            teeth.push_back({std::min(e.u, e.v), std::max(e.u, e.v), e.lp_col});
            sum += best_out_delta;
        }
    }
    std::sort(teeth.begin(), teeth.end(), [](const BlossomTooth& a, const BlossomTooth& b) {
        return std::tie(a.u, a.v) < std::tie(b.u, b.v);
    });
    return 1.0 - sum;
}

std::vector<BlossomCut> separate_blossom(const SupportGraph& g, const BlossomConfig& cfg, BlossomStats* stats) {
    BlossomStats st;
    st.heuristic = cfg.mode == BlossomMode::Heuristic;
    std::vector<BlossomCut> out;
    std::set<std::pair<std::vector<uint32_t>, std::vector<std::pair<uint32_t, uint32_t>>>> seen;
    auto consider = [&](const std::vector<uint32_t>& raw_handle, uint32_t& counter) {
        if (raw_handle.empty() || raw_handle.size() >= g.n) return;
        ++st.candidates;
        auto h = canonical_handle(raw_handle, g.n);
        std::vector<BlossomTooth> teeth;
        const double v = best_teeth(g, h, teeth);
        if (!(v > cfg.violation_tol)) return;
        const double check = blossom_violation(g, h, teeth);  // exact re-verification
        if (!(check > cfg.violation_tol) || teeth.size() % 2 == 0) return;
        std::vector<std::pair<uint32_t, uint32_t>> key;
        for (const auto& t : teeth) key.push_back({t.u, t.v});
        if (!seen.insert({h, key}).second) return;
        out.push_back({std::move(h), std::move(teeth), check});
        ++counter;
    };

    // Heuristic handles: components of {e : eps < x_e < 1 - eps} for several eps.
    for (double eps : {1e-6, 0.1, 0.25}) {
        std::vector<Edge> fe;
        std::vector<double> fx;
        for (const auto& e : g.edges)
            if (e.cap > eps && e.cap < 1.0 - eps) {
                fe.push_back(Edge{e.u, e.v, 0, 0, 0.0});
                fx.push_back(e.cap);
            }
        const auto fg = build_support(g.n, fe, fx, 0.0);
        for (const auto& comp : connected_components(fg))
            if (comp.size() >= 3) consider(comp, st.heuristic_cuts);
    }

    if (cfg.mode == BlossomMode::Full && g.n >= 2) {
        // Padberg-Rao: split every support edge e = (i, j) with node k_e; arcs (i, k_e) cap x_e,
        // (k_e, j) cap 1 - x_e. Odd labels: every k_e, and node v iff it is the second endpoint
        // of an odd number of edges; then |W ∩ odd| ≡ |T| (mod 2), and a minimum odd cut of value
        // < 1 is a maximally violated blossom.
        const uint32_t n = g.n, m = static_cast<uint32_t>(g.edges.size()), N = n + m;
        std::vector<uint8_t> odd(N, 0);
        SupportGraph aux;
        aux.n = N;
        std::vector<Edge> ae;
        std::vector<double> ax;
        for (uint32_t k = 0; k < m; ++k) {
            const auto& e = g.edges[k];
            odd[n + k] = 1;
            odd[e.v] ^= 1;
            ae.push_back(Edge{std::min(e.u, n + k), std::max(e.u, n + k), 0, 0, 0.0});
            ax.push_back(e.cap);
            ae.push_back(Edge{std::min(e.v, n + k), std::max(e.v, n + k), 0, 0, 0.0});
            ax.push_back(1.0 - e.cap);
        }
        aux = build_support(N, ae, ax, 0.0);
        // Gusfield's algorithm: Gomory-Hu cut tree with N - 1 max flows.
        std::vector<uint32_t> parent(N, 0);
        std::vector<double> flow(N, 0.0);
        Dinic dinic(aux);
        std::vector<uint8_t> side(N);
        for (uint32_t s = 1; s < N; ++s) {
            const auto cut = dinic.min_cut_between(s, parent[s]);
            flow[s] = cut.value;
            std::fill(side.begin(), side.end(), 0);
            for (uint32_t v : cut.side) side[v] = 1;
            for (uint32_t t = s + 1; t < N; ++t)
                if (side[t] && parent[t] == parent[s]) parent[t] = s;
        }
        // Subtree parity per tree edge (s, parent[s]); tree is rooted at 0.
        std::vector<std::vector<uint32_t>> children(N);
        for (uint32_t s = 1; s < N; ++s) children[parent[s]].push_back(s);
        std::vector<uint32_t> order{0};
        for (size_t k = 0; k < order.size(); ++k)
            for (uint32_t c : children[order[k]]) order.push_back(c);
        std::vector<uint32_t> odd_count(N, 0);
        for (size_t k = order.size(); k-- > 0;) {
            const uint32_t v = order[k];
            odd_count[v] += odd[v];
            if (v != 0) odd_count[parent[v]] += odd_count[v];
        }
        for (uint32_t s = 1; s < N; ++s) {
            if (odd_count[s] % 2 == 0 || flow[s] >= 1.0 - cfg.violation_tol) continue;
            std::vector<uint32_t> handle, stack{s};  // original nodes in the subtree of s
            while (!stack.empty()) {
                const uint32_t v = stack.back();
                stack.pop_back();
                if (v < n) handle.push_back(v);
                for (uint32_t c : children[v]) stack.push_back(c);
            }
            consider(handle, st.exact_cuts);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const BlossomCut& a, const BlossomCut& b) { return a.violation > b.violation; });
    if (stats) *stats = st;
    return out;
}

}  // namespace apex
