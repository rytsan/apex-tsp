#pragma once

#include <cstdint>
#include <vector>

#include "cuts/shrink.hpp"
#include "cuts/support.hpp"

namespace apex {

struct CutResult {
    double value = 0;
    std::vector<uint32_t> side;  // node set S (sorted) of the graph the cut was computed on
};

// Global minimum cut (Stoer & Wagner 1997), maximum-adjacency phases with a lazy binary heap:
// O(n m log n). Every phase cut ("cut of the phase") is also recorded: each is a valid cut,
// so all of them with value < threshold are cheap extra SEC candidates.
// ponytail: adjacency merge by list concatenation; switch to contracted CSR rebuilds if the
// unshrunken support graphs of 100k-node instances make phases too slow.
class StoerWagner {
public:
    explicit StoerWagner(const SupportGraph& g);
    CutResult run(double phase_threshold = -1.0);  // collects phase cuts with value < threshold
    const std::vector<CutResult>& phase_cuts() const { return phase_cuts_; }

private:
    uint32_t n_;
    std::vector<std::vector<std::pair<uint32_t, double>>> adj_;
    std::vector<CutResult> phase_cuts_;
};

// Dinic max-flow on a graph with directed arcs (undirected edge = two arcs).
class Dinic {
public:
    explicit Dinic(uint32_t n) : n_(n), head_(n, -1) {}
    explicit Dinic(const SupportGraph& g);
    void add_arc(uint32_t u, uint32_t v, double cap, double rev_cap = 0.0);
    double max_flow(uint32_t s, uint32_t t);
    // Max-flow value and the source side of a minimum s-t cut (nodes reachable from s in the
    // residual graph). Resets flows first, so it can be called repeatedly.
    CutResult min_cut_between(uint32_t s, uint32_t t);

private:
    struct Arc { uint32_t to; int32_t next; double cap, flow; };
    bool bfs(uint32_t s, uint32_t t);
    double dfs(uint32_t v, uint32_t t, double f);
    uint32_t n_;
    std::vector<int32_t> head_, it_;
    std::vector<int32_t> level_;
    std::vector<Arc> arcs_;
};

struct SecSeparatorConfig {
    uint32_t max_pairs_per_round = 64;  // guided pairwise Dinic budget
    double violation_tol = 1e-5;        // SEC violated when x(delta(S)) < 2 - tol
    uint64_t seed = 1;
};

struct SecSeparationStats {
    uint32_t component_cuts = 0, stoer_wagner_cuts = 0, pairwise_cuts = 0;
    uint32_t shrunk_nodes = 0;
};

// Separation strategy per round (MASTER §6, Sprint 2 M2.3):
//   1. connected components (any disconnected support);
//   2. safe shrinking + Stoer-Wagner (global min cut and all violated phase cuts);
//   3. guided pairwise Dinic min cuts between supernodes with fractional incident edges.
// Cuts are expanded to original nodes, re-verified on the original support, deduplicated
// (canonical side: fewer nodes, then the side holding the smallest node) and returned sorted by
// decreasing violation.
std::vector<SecCut> separate_sec(const SupportGraph& g, const SecSeparatorConfig& cfg = {},
                                 SecSeparationStats* stats = nullptr);

}  // namespace apex
