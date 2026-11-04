#include <doctest/doctest.h>

#include <random>
#include <vector>

#include "cuts/support.hpp"

using apex::Edge;

namespace {
Edge E(uint32_t u, uint32_t v) { return Edge{u, v, 1, apex::kActive, 0.0}; }
}  // namespace

TEST_CASE("T_components_disconnected_integral") {
    // Integral LP solution: 3-cycle {0,1,2} + 4-cycle {3,4,5,6}, plus zero-valued edges
    // between them (present in the LP, absent from the support).
    const std::vector<Edge> edges = {E(0, 1), E(1, 2), E(0, 2), E(3, 4), E(4, 5), E(5, 6), E(3, 6), E(2, 3), E(1, 5)};
    const std::vector<double> x = {1, 1, 1, 1, 1, 1, 1, 0, 0};
    const auto g = apex::build_support(7, edges, x);
    CHECK(g.edges.size() == 7);
    const auto comps = apex::connected_components(g);
    REQUIRE(comps.size() == 2);
    CHECK(comps[0] == std::vector<uint32_t>{0, 1, 2});
    CHECK(comps[1] == std::vector<uint32_t>{3, 4, 5, 6});
    const auto cuts = apex::component_cuts(g);
    REQUIRE(cuts.size() == 2);
    for (const auto& c : cuts) CHECK(c.violation == 2.0);  // x(delta(S)) = 0

    // Connected solution (Hamiltonian cycle 0..6): no component cut.
    const std::vector<Edge> ring = {E(0, 1), E(1, 2), E(2, 3), E(3, 4), E(4, 5), E(5, 6), E(0, 6)};
    const auto gr = apex::build_support(7, ring, std::vector<double>(7, 1.0));
    CHECK(apex::component_cuts(gr).empty());
}

TEST_CASE("T_components_fractional_and_isolated") {
    // Fractional disconnected solution: two triangles at 0.5 and a doubled edge, isolated node 7.
    const std::vector<Edge> edges = {E(0, 1), E(1, 2), E(0, 2), E(3, 4), E(4, 5), E(3, 5), E(5, 6)};
    const std::vector<double> x = {0.5, 0.5, 0.5, 1, 1, 0.5, 0.5};
    const auto g = apex::build_support(8, edges, x);
    const auto cuts = apex::component_cuts(g);
    CHECK(cuts.size() == 3);  // {0,1,2}, {3,4,5,6}, {7}
    CHECK(apex::cut_value(g, {0, 1}) == doctest::Approx(1.0));
    CHECK(apex::cut_value(g, {3, 4, 5}) == doctest::Approx(0.5));
}

TEST_CASE("T_components_large_no_recursion") {
    // 200k-node path split in two halves: iterative DFS must not overflow the stack.
    const uint32_t n = 200000;
    std::vector<Edge> edges;
    std::vector<double> x;
    for (uint32_t i = 0; i + 1 < n; ++i) {
        edges.push_back(E(i, i + 1));
        x.push_back(i == n / 2 ? 0.0 : 1.0);
    }
    const auto comps = apex::connected_components(apex::build_support(n, edges, x));
    REQUIRE(comps.size() == 2);
    CHECK(comps[0].size() == n / 2 + 1);
}

TEST_CASE("T_support_capacity_exact") {
    std::mt19937_64 rng(1);
    std::uniform_real_distribution<double> U(0, 1);
    std::vector<Edge> edges;
    std::vector<double> x;
    for (uint32_t k = 0; k < 500; ++k) {
        const uint32_t u = rng() % 100, v = (u + 1 + rng() % 99) % 100;
        edges.push_back(E(std::min(u, v), std::max(u, v)));
        const double r = U(rng);
        x.push_back(r < 0.3 ? 0.0 : r < 0.35 ? 1e-10 : r);  // zeros and below-tolerance values
    }
    const auto g = apex::build_support(100, edges, x, 1e-9);
    size_t expected = 0;
    for (double v : x) expected += v > 1e-9;
    CHECK(g.edges.size() == expected);
    size_t mismatches = 0;
    for (const auto& e : g.edges) {
        mismatches += e.cap != x[e.lp_col];  // bit-exact
        mismatches += e.u != edges[e.lp_col].u || e.v != edges[e.lp_col].v;
    }
    CHECK(mismatches == 0);
    CHECK(g.adj.size() == 2 * g.edges.size());
}
