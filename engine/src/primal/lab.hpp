#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/instance.hpp"
#include "graph/candidate_graph.hpp"
#include "graph/tours.hpp"

namespace apex {

// Heuristics lab (Sprint 6 M6.4): one named step on the current tour, from the existing primal
// code only (no new solver semantics). Steps:
//   construction (replace the tour): nn, greedy, hilbert, random, decomp
//   improvement (start from the tour): two_opt, two_opt_oropt, lk, ils [seconds]
// UB only: every result is a heuristic tour.
struct LabStep {
    std::string name;
    bool ok = false;
    std::string error;      // unknown step / bad argument
    int64_t before = 0, after = 0;
    double ms = 0;
    uint64_t moves = 0;     // 2-opt / Or-opt / LK moves or ILS improvements
};

const std::vector<std::string>& lab_step_names();

// Nearest neighbour from node `start`: candidate-graph neighbours first, else the nearest
// unvisited node found by an expanding grid search.
Tour nearest_neighbor_tour(const Instance& inst, const CandidateGraph& g, uint32_t start = 0);
Tour random_tour(const Instance& inst, uint64_t seed);

// `command` = "<step> [arg]". on_tour gets intermediate tours (ILS, throttled by the caller).
LabStep run_lab_step(const std::string& command, Tour& tour, const Instance& inst, const CandidateGraph& g, uint64_t seed,
                     uint32_t threads = 1, const std::function<void(const Tour&)>& on_tour = nullptr);

}  // namespace apex
