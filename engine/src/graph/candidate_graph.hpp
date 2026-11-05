#pragma once

#include <cstdint>
#include <vector>

#include "core/instance.hpp"

namespace apex {

enum EdgeStatus : uint32_t {
    kFixed0 = 0x01,
    kFixed1 = 0x02,
    kActive = 0x04,
    kRcfPruned = 0x08,
    kNewPricing = 0x10,
};

struct Edge {               // 24 B, 8-byte aligned (MASTER §5.2)
    uint32_t u, v;          // u < v
    int32_t cost;           // TSPLIB metric, computed in double
    uint32_t status;
    double reduced_cost;    // double: float corrupts RCF decisions
};
static_assert(sizeof(Edge) == 24, "Edge layout changed");

struct CandidateConfig {
    uint32_t knn = 8;        // --knn
    bool delaunay = true;    // disabled only by tests exercising the fallback
};

struct CandidateGraph {
    uint32_t n = 0;
    std::vector<Edge> edges;          // sorted by (u, v), unique
    // CSR adjacency: neighbours of i are adj[adj_start[i] .. adj_start[i+1]),
    // each entry an edge index, sorted by (cost, other endpoint).
    std::vector<uint32_t> adj_start;
    std::vector<uint32_t> adj;
    uint32_t fallback_edges = 0;      // MST edges added to restore connectivity

    uint32_t other(uint32_t e, uint32_t i) const { return edges[e].u == i ? edges[e].v : edges[e].u; }
};

// Delaunay ∪ symmetric k-NN, dedup by canonical pair, status = kActive.
// If disconnected, adds minimum-cost connecting edges (Kruskal over growing k-NN).
CandidateGraph build_candidate_graph(const Instance& inst, const CandidateConfig& cfg = {});

// Recomputes the CSR adjacency (cost-sorted) after edges were appended (e.g. by pricing).
void rebuild_adjacency(CandidateGraph& g);

}  // namespace apex
