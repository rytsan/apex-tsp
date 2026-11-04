#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <numeric>
#include <string>
#include <vector>

#include "core/instance.hpp"
#include "geo/grid.hpp"
#include "graph/candidate_graph.hpp"

namespace {

bool connected(const apex::CandidateGraph& g) {
    if (g.n == 0) return true;
    std::vector<uint8_t> seen(g.n, 0);
    std::vector<uint32_t> stack{0};
    seen[0] = 1;
    uint32_t count = 1;
    while (!stack.empty()) {
        const uint32_t i = stack.back();
        stack.pop_back();
        for (uint32_t p = g.adj_start[i]; p < g.adj_start[i + 1]; ++p) {
            const uint32_t j = g.other(g.adj[p], i);
            if (!seen[j]) { seen[j] = 1; ++count; stack.push_back(j); }
        }
    }
    return count == g.n;
}

// Symmetry, no self-loops, no duplicates, costs canonical, adjacency sorted.
void check_symmetry(const apex::Instance& in, const apex::CandidateGraph& g) {
    size_t bad = 0;
    for (size_t e = 0; e < g.edges.size(); ++e) {
        const auto& ed = g.edges[e];
        bad += !(ed.u < ed.v);                                     // canonical, no self-loop
        bad += ed.cost != apex::dist(in, ed.u, ed.v);
        bad += ed.status != apex::kActive;
        if (e > 0) bad += !(std::make_pair(g.edges[e - 1].u, g.edges[e - 1].v) < std::make_pair(ed.u, ed.v));
    }
    CHECK(bad == 0);
    CHECK(g.adj.size() == 2 * g.edges.size());
    std::vector<uint32_t> hits(g.edges.size(), 0);
    size_t bad_adj = 0;
    for (uint32_t i = 0; i < g.n; ++i) {
        for (uint32_t p = g.adj_start[i]; p < g.adj_start[i + 1]; ++p) {
            const uint32_t e = g.adj[p];
            bad_adj += g.edges[e].u != i && g.edges[e].v != i;
            ++hits[e];
            if (p > g.adj_start[i]) bad_adj += g.edges[g.adj[p - 1]].cost > g.edges[e].cost;
        }
    }
    CHECK(bad_adj == 0);
    CHECK(std::all_of(hits.begin(), hits.end(), [](uint32_t h) { return h == 2; }));  // both directions
}

}  // namespace

TEST_CASE("T_euc2d_reference_table") {
    CHECK(apex::euc2d(0, 0, 3, 4) == 5);
    CHECK(apex::euc2d(0, 0, 1, 1) == 1);        // 1.414
    CHECK(apex::euc2d(0, 0, 1, 2) == 2);        // 2.236
    CHECK(apex::euc2d(0, 0, 2.5, 0) == 3);      // .5 rounds up
    CHECK(apex::euc2d(0, 0, 2.4999, 0) == 2);
    CHECK(apex::euc2d(0, 0, 0, 0) == 0);
    CHECK(apex::euc2d(-1e6, 0, 1e6, 0) == 2000000);
    CHECK(apex::ceil2d(0, 0, 1, 1) == 2);
    CHECK(apex::ceil2d(0, 0, 3, 4) == 5);
    CHECK(apex::ceil2d(0, 0, 0.0001, 0) == 1);
    // berlin52 nodes 1 and 2: (565,575) (25,185) -> sqrt(443700) = 666.108 -> 666
    CHECK(apex::euc2d(565, 575, 25, 185) == 666);
}

TEST_CASE("T_candidate_graph_symmetry") {
    const auto in = apex::read_tsplib(std::string(APEX_DATA_DIR) + "/a280.tsp");  // has a duplicate point
    for (uint32_t k : {1u, 8u, 16u}) {
        CAPTURE(k);
        const auto g = apex::build_candidate_graph(in, {k, true});
        check_symmetry(in, g);
        // symmetric k-NN: every j in kNN(i) is adjacent to i.
        const apex::SpatialGrid grid(in);
        std::vector<uint32_t> nb;
        size_t missing = 0;
        for (uint32_t i = 0; i < in.n; ++i) {
            grid.knn(i, k, nb);
            for (uint32_t j : nb) {
                bool found = false;
                for (uint32_t p = g.adj_start[i]; p < g.adj_start[i + 1] && !found; ++p) found = g.other(g.adj[p], i) == j;
                missing += !found;
            }
        }
        CHECK(missing == 0);
    }
}

TEST_CASE("T_candidate_graph_fallback") {
    // Two far-apart clusters, no Delaunay, k=2: k-NN graph is disconnected.
    apex::Instance in;
    in.name = "clusters";
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 10; ++i) {
            in.xs.push_back(c * 1e6 + i);
            in.ys.push_back((i % 3) * 1.0);
        }
    in.n = static_cast<uint32_t>(in.xs.size());
    in.original_ids.resize(in.n);
    std::iota(in.original_ids.begin(), in.original_ids.end(), 1u);

    const auto plain = apex::build_candidate_graph(in, {2, false});
    CHECK(connected(plain));
    CHECK(plain.fallback_edges >= 2);
    check_symmetry(in, plain);
    MESSAGE("fallback_edges=" << plain.fallback_edges);
    const auto with_dt = apex::build_candidate_graph(in, {2, true});
    CHECK(with_dt.fallback_edges == 0);
    CHECK(connected(with_dt));
}

TEST_CASE("T_candidate_graph_connected") {
    for (const char* name : {"eil51", "berlin52", "a280", "pr1002", "fl3795", "usa13509", "d15112", "d18512",
                             "pla85900", "mona-lisa100K"}) {
        CAPTURE(name);
        const auto in = apex::read_tsplib(std::string(APEX_DATA_DIR) + "/" + name + ".tsp");
        const auto t0 = std::chrono::steady_clock::now();
        const auto g = apex::build_candidate_graph(in);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        MESSAGE(std::string(name) << " n=" << in.n << " edges=" << g.edges.size() << " avg_deg="
                                  << 2.0 * g.edges.size() / in.n << " fallback=" << g.fallback_edges << " ms=" << ms);
        CHECK(connected(g));
        check_symmetry(in, g);
    }
}
