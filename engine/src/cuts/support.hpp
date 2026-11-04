#pragma once

#include <cstdint>
#include <vector>

#include "graph/candidate_graph.hpp"

namespace apex {

struct SupportEdge {
    uint32_t u, v;
    double cap;      // = x_e (exact LP value)
    uint32_t lp_col; // column of the LP variable
};

// Support graph of an LP solution: edges with x_e > tol, CSR adjacency (edge ids).
struct SupportGraph {
    uint32_t n = 0;
    std::vector<SupportEdge> edges;
    std::vector<uint32_t> adj_start, adj;

    uint32_t other(uint32_t e, uint32_t i) const { return edges[e].u == i ? edges[e].v : edges[e].u; }
};

// edges[j] are the endpoints of LP column j; x is the LP solution (same indexing).
SupportGraph build_support(uint32_t n, const std::vector<Edge>& edges, const std::vector<double>& x, double tol = 1e-9);

// Connected components (iterative DFS); each component sorted ascending, components ordered
// by smallest node.
std::vector<std::vector<uint32_t>> connected_components(const SupportGraph& g);

// x(delta(S)) on the support graph.
double cut_value(const SupportGraph& g, const std::vector<uint32_t>& S);

struct SecCut {
    std::vector<uint32_t> nodes;  // sorted node set S (original node ids)
    double violation;             // 2 - x(delta(S))
};

// One SEC per component when the support graph is disconnected (none if connected).
// Uses the smaller side canonically: S and V\S define the same SEC.
std::vector<SecCut> component_cuts(const SupportGraph& g);

}  // namespace apex
