#include "cuts/shrink.hpp"

#include <algorithm>
#include <numeric>
#include <tuple>

namespace apex {
namespace {

struct Dsu {
    std::vector<uint32_t> p;
    explicit Dsu(uint32_t n) : p(n) { std::iota(p.begin(), p.end(), 0u); }
    uint32_t find(uint32_t x) {
        while (p[x] != x) x = p[x] = p[p[x]];
        return x;
    }
    bool unite(uint32_t a, uint32_t b) {
        a = find(a);
        b = find(b);
        if (a == b) return false;
        p[std::max(a, b)] = std::min(a, b);
        return true;
    }
};

}  // namespace

std::vector<uint32_t> ShrunkGraph::expand(const std::vector<uint32_t>& supernodes) const {
    std::vector<uint32_t> out;
    for (uint32_t s : supernodes) out.insert(out.end(), members[s].begin(), members[s].end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

ShrunkGraph shrink_unit_edges(const SupportGraph& g, double tol) {
    Dsu dsu(g.n);
    constexpr double kDegTol = 1e-6;  // "degree is 2" tolerance for LP round-off
    std::vector<double> deg(g.n);
    std::vector<uint8_t> busy(g.n);
    for (;;) {
        // Aggregate edges between current supernodes (sum parallel edges, drop self-loops).
        std::vector<std::tuple<uint32_t, uint32_t, double>> agg;
        agg.reserve(g.edges.size());
        for (const auto& e : g.edges) {
            uint32_t a = dsu.find(e.u), b = dsu.find(e.v);
            if (a == b) continue;
            if (a > b) std::swap(a, b);
            agg.emplace_back(a, b, e.cap);
        }
        std::sort(agg.begin(), agg.end());
        // Collapse parallel edges and compute exact supernode degrees x(delta(A)).
        size_t w = 0;
        for (size_t k = 0; k < agg.size();) {
            const uint32_t a = std::get<0>(agg[k]), b = std::get<1>(agg[k]);
            double c = 0;
            for (; k < agg.size() && std::get<0>(agg[k]) == a && std::get<1>(agg[k]) == b; ++k) c += std::get<2>(agg[k]);
            agg[w++] = {a, b, c};
        }
        agg.resize(w);
        std::fill(deg.begin(), deg.end(), 0.0);
        for (const auto& [a, b, c] : agg) { deg[a] += c; deg[b] += c; }
        // Merge a matching of eligible pairs: c >= 1 and BOTH degrees == 2. Disjoint merges do
        // not change each other's degrees or capacities, so each stays safe.
        std::fill(busy.begin(), busy.end(), uint8_t{0});
        bool merged = false;
        for (const auto& [a, b, c] : agg) {
            if (c < 1.0 - tol || busy[a] || busy[b]) continue;
            if (deg[a] < 2.0 - kDegTol || deg[b] < 2.0 - kDegTol) continue;
            busy[a] = busy[b] = 1;
            dsu.unite(a, b);
            merged = true;
        }
        if (merged) continue;

        // Final: relabel supernodes densely (ordered by smallest original member).
        ShrunkGraph s;
        s.super_of.assign(g.n, 0);
        std::vector<int64_t> label(g.n, -1);
        uint32_t next = 0;
        for (uint32_t v = 0; v < g.n; ++v) {
            const uint32_t r = dsu.find(v);
            if (label[r] < 0) { label[r] = next++; s.members.emplace_back(); }
            s.super_of[v] = static_cast<uint32_t>(label[r]);
            s.members[s.super_of[v]].push_back(v);
        }
        s.g.n = next;
        for (size_t k = 0; k < agg.size();) {
            const uint32_t a = std::get<0>(agg[k]), b = std::get<1>(agg[k]);
            double c = 0;
            for (; k < agg.size() && std::get<0>(agg[k]) == a && std::get<1>(agg[k]) == b; ++k) c += std::get<2>(agg[k]);
            uint32_t la = static_cast<uint32_t>(label[a]), lb = static_cast<uint32_t>(label[b]);
            if (la > lb) std::swap(la, lb);
            s.g.edges.push_back({la, lb, c, UINT32_MAX});
        }
        std::sort(s.g.edges.begin(), s.g.edges.end(),
                  [](const SupportEdge& x, const SupportEdge& y) { return std::tie(x.u, x.v) < std::tie(y.u, y.v); });
        s.g.adj_start.assign(next + 1, 0);
        for (const auto& e : s.g.edges) {
            ++s.g.adj_start[e.u + 1];
            ++s.g.adj_start[e.v + 1];
        }
        for (uint32_t i = 0; i < next; ++i) s.g.adj_start[i + 1] += s.g.adj_start[i];
        s.g.adj.resize(2 * s.g.edges.size());
        std::vector<uint32_t> fill(s.g.adj_start.begin(), s.g.adj_start.end() - 1);
        for (uint32_t e = 0; e < s.g.edges.size(); ++e) {
            s.g.adj[fill[s.g.edges[e].u]++] = e;
            s.g.adj[fill[s.g.edges[e].v]++] = e;
        }
        return s;
    }
}

}  // namespace apex
