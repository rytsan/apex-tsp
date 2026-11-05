#include "primal/lp_tour.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <tuple>

#include "primal/two_opt.hpp"

namespace apex {

Tour lp_guided_tour(const Instance& inst, const CandidateGraph& g, const std::vector<double>& x) {
    const uint32_t n = inst.n;
    std::vector<uint32_t> parent(n), deg(n, 0);
    std::iota(parent.begin(), parent.end(), 0u);
    auto find = [&](uint32_t v) {
        while (parent[v] != v) v = parent[v] = parent[parent[v]];
        return v;
    };
    std::vector<std::vector<uint32_t>> adj(n);
    uint32_t added = 0;
    auto try_add = [&](uint32_t u, uint32_t v) {
        if (deg[u] >= 2 || deg[v] >= 2) return;
        const uint32_t a = find(u), b = find(v);
        if (a == b) return;
        parent[a] = b;
        ++deg[u];
        ++deg[v];
        adj[u].push_back(v);
        adj[v].push_back(u);
        ++added;
    };
    std::vector<uint32_t> order(g.edges.size());
    std::iota(order.begin(), order.end(), 0u);
    auto xv = [&](uint32_t j) { return j < x.size() ? x[j] : 0.0; };
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return std::make_tuple(-xv(a), g.edges[a].cost, a) < std::make_tuple(-xv(b), g.edges[b].cost, b);
    });
    for (uint32_t j : order)  // LP support first, then the rest of the candidate graph by cost
        if (xv(j) > 1e-6) try_add(g.edges[j].u, g.edges[j].v);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return std::make_pair(g.edges[a].cost, a) < std::make_pair(g.edges[b].cost, b);
    });
    for (uint32_t j : order) {
        if (added + 1 >= n) break;
        try_add(g.edges[j].u, g.edges[j].v);
    }

    // Path fragments (an isolated node is a fragment with both ends equal): walk each, then
    // jump to the nearest free endpoint of an unvisited fragment.
    // ponytail: O(k^2) over k fragments; k is small after the candidate-graph completion.
    std::vector<uint32_t> ends;
    for (uint32_t v = 0; v < n; ++v)
        if (deg[v] < 2) ends.push_back(v);
    std::vector<uint8_t> seen(n, 0);
    Tour t;
    t.order.reserve(n);
    auto walk = [&](uint32_t start) {  // appends the fragment from `start`; returns its other end
        uint32_t prev = UINT32_MAX, cur = start;
        for (;;) {
            seen[cur] = 1;
            t.order.push_back(cur);
            uint32_t next = UINT32_MAX;
            for (uint32_t w : adj[cur])
                if (w != prev && !seen[w]) next = w;
            if (next == UINT32_MAX) return cur;
            prev = cur;
            cur = next;
        }
    };
    if (ends.empty()) {  // already a Hamiltonian path closed? impossible with the DSU; defensive
        walk(0);
    } else {
        uint32_t tail = walk(ends[0]);
        for (;;) {
            uint32_t best = UINT32_MAX;
            int32_t bd = INT32_MAX;
            for (uint32_t e : ends)
                if (!seen[e]) {
                    const int32_t d = dist(inst, tail, e);
                    if (d < bd) { bd = d; best = e; }
                }
            if (best == UINT32_MAX) break;
            tail = walk(best);
        }
    }
    t.cost = tour_cost(inst, t);
    two_opt_oropt(t, inst, g, TwoOptConfig{});
    return t;
}

}  // namespace apex
