#pragma once

#include <vector>

#include "core/instance.hpp"
#include "graph/candidate_graph.hpp"
#include "graph/tours.hpp"

namespace apex {

// LP-guided tour (heuristic, UB only): greedy edge selection over the LP columns by x
// (descending; ties: cost, index), completed greedily by candidate-graph cost, remaining path
// fragments joined nearest-endpoint first, then 2-opt + Or-opt on the candidate graph.
// x: LP solution over g.edges (shorter vectors are padded with 0). With x empty and
// local_search false it is the plain greedy-edge construction (heuristics lab).
Tour lp_guided_tour(const Instance& inst, const CandidateGraph& g, const std::vector<double>& x, bool local_search = true);

}  // namespace apex
