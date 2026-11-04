#include "graph/candidate_graph.hpp"

#include <algorithm>
#include <numeric>
#include <tuple>
#include <utility>

#include "geo/delaunay.hpp"
#include "geo/grid.hpp"

namespace apex {
namespace {

struct Dsu {
    std::vector<uint32_t> p;
    uint32_t sets;
    explicit Dsu(uint32_t n) : p(n), sets(n) { std::iota(p.begin(), p.end(), 0u); }
    uint32_t find(uint32_t x) {
        while (p[x] != x) x = p[x] = p[p[x]];
        return x;
    }
    bool unite(uint32_t a, uint32_t b) {
        a = find(a);
        b = find(b);
        if (a == b) return false;
        p[std::max(a, b)] = std::min(a, b);
        --sets;
        return true;
    }
};

using Pair = std::pair<uint32_t, uint32_t>;

void add_knn(const Instance& inst, const SpatialGrid& grid, uint32_t k, std::vector<Pair>& out) {
    std::vector<uint32_t> nb;
    for (uint32_t i = 0; i < inst.n; ++i) {
        grid.knn(i, k, nb);
        for (uint32_t j : nb) out.emplace_back(std::min(i, j), std::max(i, j));
    }
}

void sort_unique(std::vector<Pair>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

}  // namespace

CandidateGraph build_candidate_graph(const Instance& inst, const CandidateConfig& cfg) {
    CandidateGraph g;
    g.n = inst.n;
    const SpatialGrid grid(inst);

    std::vector<Pair> pairs;
    if (cfg.delaunay) pairs = delaunay(inst).edges;
    add_knn(inst, grid, cfg.knn, pairs);  // (min, max): j in kNN(i) enters both adjacency lists
    sort_unique(pairs);

    // Connectivity fallback: Kruskal over k-NN graphs of growing k, keeping only
    // edges that join different components (MST-forest edges).
    Dsu dsu(inst.n);
    for (auto [u, v] : pairs) dsu.unite(u, v);
    for (uint32_t k = std::max(2u, 2 * cfg.knn); dsu.sets > 1 && inst.n > 1; k = std::min(inst.n - 1, 2 * k)) {
        std::vector<Pair> extra;
        add_knn(inst, grid, k, extra);
        sort_unique(extra);
        std::vector<std::tuple<int32_t, uint32_t, uint32_t>> by_cost;
        for (auto [u, v] : extra)
            if (dsu.find(u) != dsu.find(v)) by_cost.emplace_back(dist(inst, u, v), u, v);
        std::sort(by_cost.begin(), by_cost.end());
        for (auto [c, u, v] : by_cost)
            if (dsu.unite(u, v)) {
                pairs.emplace_back(u, v);
                ++g.fallback_edges;
            }
        if (k == inst.n - 1) break;  // complete graph considered: must be connected now
    }
    sort_unique(pairs);

    g.edges.reserve(pairs.size());
    for (auto [u, v] : pairs) g.edges.push_back(Edge{u, v, dist(inst, u, v), kActive, 0.0});

    g.adj_start.assign(inst.n + 1, 0);
    for (const auto& e : g.edges) {
        ++g.adj_start[e.u + 1];
        ++g.adj_start[e.v + 1];
    }
    for (uint32_t i = 0; i < inst.n; ++i) g.adj_start[i + 1] += g.adj_start[i];
    g.adj.resize(2 * g.edges.size());
    std::vector<uint32_t> fill(g.adj_start.begin(), g.adj_start.end() - 1);
    for (uint32_t e = 0; e < g.edges.size(); ++e) {
        g.adj[fill[g.edges[e].u]++] = e;
        g.adj[fill[g.edges[e].v]++] = e;
    }
    for (uint32_t i = 0; i < inst.n; ++i) {
        std::sort(g.adj.begin() + g.adj_start[i], g.adj.begin() + g.adj_start[i + 1], [&](uint32_t a, uint32_t b) {
            return std::make_pair(g.edges[a].cost, g.other(a, i)) < std::make_pair(g.edges[b].cost, g.other(b, i));
        });
    }
    return g;
}

}  // namespace apex
