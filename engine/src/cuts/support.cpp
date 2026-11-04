#include "cuts/support.hpp"

#include <algorithm>
#include <stdexcept>

namespace apex {

SupportGraph build_support(uint32_t n, const std::vector<Edge>& edges, const std::vector<double>& x, double tol) {
    if (x.size() != edges.size()) throw std::invalid_argument("build_support: x and edges size mismatch");
    SupportGraph g;
    g.n = n;
    for (uint32_t j = 0; j < edges.size(); ++j)
        if (x[j] > tol) g.edges.push_back({edges[j].u, edges[j].v, x[j], j});
    g.adj_start.assign(n + 1, 0);
    for (const auto& e : g.edges) {
        ++g.adj_start[e.u + 1];
        ++g.adj_start[e.v + 1];
    }
    for (uint32_t i = 0; i < n; ++i) g.adj_start[i + 1] += g.adj_start[i];
    g.adj.resize(2 * g.edges.size());
    std::vector<uint32_t> fill(g.adj_start.begin(), g.adj_start.end() - 1);
    for (uint32_t e = 0; e < g.edges.size(); ++e) {
        g.adj[fill[g.edges[e].u]++] = e;
        g.adj[fill[g.edges[e].v]++] = e;
    }
    return g;
}

std::vector<std::vector<uint32_t>> connected_components(const SupportGraph& g) {
    std::vector<std::vector<uint32_t>> comps;
    std::vector<uint8_t> seen(g.n, 0);
    std::vector<uint32_t> stack;
    for (uint32_t s = 0; s < g.n; ++s) {
        if (seen[s]) continue;
        comps.emplace_back();
        auto& comp = comps.back();
        seen[s] = 1;
        stack.push_back(s);
        while (!stack.empty()) {
            const uint32_t i = stack.back();
            stack.pop_back();
            comp.push_back(i);
            for (uint32_t p = g.adj_start[i]; p < g.adj_start[i + 1]; ++p) {
                const uint32_t j = g.other(g.adj[p], i);
                if (!seen[j]) { seen[j] = 1; stack.push_back(j); }
            }
        }
        std::sort(comp.begin(), comp.end());
    }
    return comps;
}

double cut_value(const SupportGraph& g, const std::vector<uint32_t>& S) {
    std::vector<uint8_t> in(g.n, 0);
    for (uint32_t v : S) in[v] = 1;
    double c = 0;
    for (uint32_t v : S)
        for (uint32_t p = g.adj_start[v]; p < g.adj_start[v + 1]; ++p)
            if (!in[g.other(g.adj[p], v)]) c += g.edges[g.adj[p]].cap;
    return c;
}

std::vector<SecCut> component_cuts(const SupportGraph& g) {
    std::vector<SecCut> cuts;
    const auto comps = connected_components(g);
    if (comps.size() <= 1) return cuts;
    for (const auto& c : comps) {
        // A component has x(delta(S)) = 0 on the support graph; keep S as is (|S| <= n/2 is
        // not required for validity, and every component yields a distinct cut).
        cuts.push_back({c, 2.0 - cut_value(g, c)});
    }
    return cuts;
}

}  // namespace apex
