#pragma once

#include <cstdint>
#include <vector>

#include "core/instance.hpp"
#include "graph/candidate_graph.hpp"
#include "graph/tours.hpp"

namespace apex {

struct TwoOptConfig {
    uint32_t initial_neighbors = 10;  // neighbour-list size of the first round
    uint32_t max_neighbors = 0;       // 0 = max candidate-graph degree
    bool or_opt = true;               // segments of 1..3, both orientations
    uint64_t seed = 1;                // shuffles the initial work queue
    double time_limit_s = 0;          // 0 = no limit
};

struct TwoOptStats {
    uint64_t moves_2opt = 0, moves_oropt = 0;
    uint32_t rounds = 0, final_neighbors = 0;
    bool time_limit_hit = false;
};

// UB0: visit nodes in Hilbert order.
Tour hilbert_tour(const Instance& inst, const std::vector<uint32_t>& hilbert_order);

// First-improvement 2-opt + Or-opt with don't-look bits over candidate-graph
// neighbour lists. Neighbour count starts at initial_neighbors and grows while
// a round still finds improvements. Deterministic for a given seed. Heuristic (UB only).
TwoOptStats two_opt_oropt(Tour& tour, const Instance& inst, const CandidateGraph& g, const TwoOptConfig& cfg);

}  // namespace apex
