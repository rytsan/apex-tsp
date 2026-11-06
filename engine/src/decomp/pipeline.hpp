#pragma once

// heuristic: primal pipeline (M4.7). UB construction and improvement only; nothing here is a bound.
//   construct: density clusters solved in parallel, merged level by level with re-evaluation
//              (n > 2 * leaf size), else greedy edge + 2-opt / Or-opt;
//   polish:    ILS (threads > 1 on a small instance: one independent ILS per thread, different
//              seeds, best kept); with threads > 1 and n >= 2 * segment size, the tour is cut into segments
//              of consecutive nodes, each improved in parallel as a path with fixed endpoints,
//              segment boundaries shifted by half a segment every round;
//   then the tour edges join the candidate graph and the tour is the UB the exact mode starts from.

#include <cstdint>

#include "decomp/patching.hpp"
#include "graph/candidate_graph.hpp"
#include "graph/tours.hpp"

namespace apex {

struct ConstructStats {
    bool decomposed = false;
    DecompStats decomp;
    double seconds = 0;
};

// heuristic
Tour construct_tour(const Instance& inst, const CandidateGraph& g, const DecompConfig& cfg, ConstructStats* stats = nullptr);

struct PolishConfig {
    uint32_t threads = 1;
    uint32_t segment_size = 10000;    // nodes per parallel segment
    double time_limit_s = 0;          // total budget (0: use rounds x iterations_per_node)
    uint32_t rounds = 0;              // 0 with a time limit: until the budget is spent
    uint64_t iterations_per_node = 2; // per round, when no time limit (deterministic)
    uint64_t stall_per_node = 0;      // whole-tour ILS stops after stall_per_node * n idle iterations (0 = off)
    uint64_t seed = 1;
};

struct PolishStats {
    uint32_t rounds = 0, segments = 0;
    uint64_t iterations = 0;
    int64_t start_cost = 0, final_cost = 0;
    double seconds = 0;
    bool parallel = false;
};

// heuristic
PolishStats polish_tour(Tour& tour, const Instance& inst, const CandidateGraph& g, const PolishConfig& cfg);

// Adds the tour edges missing from g (status kActive) and rebuilds the adjacency. Returns the count.
uint32_t add_tour_edges(CandidateGraph& g, const Instance& inst, const Tour& t);

}  // namespace apex
