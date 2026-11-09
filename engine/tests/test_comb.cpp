#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <sstream>
#include <vector>

#include "cuts/comb.hpp"
#include "cuts/pool.hpp"

using apex::Edge;

namespace {

// Blossom gadget (handle triangle a,b,c x = 0.6; teeth x = 0.8; outer triangle 0.6) with every
// gadget vertex g split into a chain g1 - g2 (x = 1). Each tooth edge becomes two parallel 0.4
// edges, so no crossing edge has x > 1/2 and no two-node-tooth blossom is violated; the comb
// with handle {a1,a2,b1,b2,c1,c2} and teeth {g1,g2,g'1,g'2} has lhs 2.4 + 3 * 2.4 = 9.6 < 10.
// Nodes: a=0,1 b=2,3 c=4,5 a'=6,7 b'=8,9 c'=10,11 (g1 = 2i, g2 = 2i+1).
struct Point {
    uint32_t n = 12;
    std::vector<Edge> edges;
    std::vector<double> x;
};

Point chain_gadget() {
    std::map<std::pair<uint32_t, uint32_t>, double> acc;
    auto add = [&](uint32_t a, uint32_t b, double w) { acc[{std::min(a, b), std::max(a, b)}] += w; };
    for (uint32_t g = 0; g < 6; ++g) add(2 * g, 2 * g + 1, 1.0);
    for (uint32_t base : {0u, 6u}) {  // triangles: g1 -> next g2
        add(base + 0, base + 3, 0.6);
        add(base + 2, base + 5, 0.6);
        add(base + 4, base + 1, 0.6);
    }
    for (uint32_t g = 0; g < 3; ++g) {  // teeth
        add(2 * g, 2 * g + 6, 0.4);
        add(2 * g + 1, 2 * g + 7, 0.4);
    }
    Point p;
    for (const auto& [k, w] : acc) {
        p.edges.push_back(Edge{k.first, k.second, 1, apex::kActive, 0.0});
        p.x.push_back(w);
    }
    return p;
}

double tour_lhs(const std::vector<uint32_t>& tour, const apex::CombCut& c, uint32_t n) {
    std::vector<Edge> e;
    std::vector<double> x;
    for (size_t i = 0; i < tour.size(); ++i) {
        const uint32_t a = tour[i], b = tour[(i + 1) % tour.size()];
        e.push_back(Edge{std::min(a, b), std::max(a, b), 1, apex::kActive, 0.0});
        x.push_back(1.0);
    }
    const auto g = apex::build_support(n, e, x);
    return 3.0 * static_cast<double>(c.teeth.size()) + 1.0 - apex::comb_violation(g, c.handle, c.teeth);
}

}  // namespace

TEST_CASE("T_comb_valid_structure") {
    using V = std::vector<uint32_t>;
    CHECK(apex::comb_valid(12, V{0, 1, 2}, {V{0, 3}, V{1, 4}, V{2, 5}}));
    CHECK_FALSE(apex::comb_valid(12, V{0, 1, 2}, {V{0, 3}, V{1, 4}}));             // k even
    CHECK_FALSE(apex::comb_valid(12, V{0, 1, 2}, {V{0, 3}, V{1, 3}, V{2, 5}}));    // teeth overlap
    CHECK_FALSE(apex::comb_valid(12, V{0, 1, 2}, {V{0, 1}, V{1, 4}, V{2, 5}}));    // tooth inside H (and overlap)
    CHECK_FALSE(apex::comb_valid(12, V{0, 1, 2}, {V{3, 6}, V{1, 4}, V{2, 5}}));    // tooth misses H
}

TEST_CASE("T_comb_separates_chain_gadget") {
    const auto p = chain_gadget();
    const auto g = apex::build_support(p.n, p.edges, p.x);
    for (uint32_t v = 0; v < p.n; ++v) {  // degree 2 everywhere
        double d = 0;
        for (size_t k = 0; k < p.edges.size(); ++k)
            if (p.edges[k].u == v || p.edges[k].v == v) d += p.x[k];
        CHECK(d == doctest::Approx(2.0));
    }
    CHECK(apex::separate_blossom(g, {apex::BlossomMode::Full}).empty());  // no edge-tooth blossom

    apex::CombStats st;
    const auto cuts = apex::separate_comb(g, {}, &st);
    REQUIRE(!cuts.empty());
    const auto& c = cuts[0];
    CHECK(c.violation == doctest::Approx(0.4));
    CHECK(c.teeth.size() == 3);
    CHECK(apex::comb_valid(p.n, c.handle, c.teeth));
    for (const auto& t : c.teeth) CHECK(t.size() == 4);

    // Valid for tours: every Hamiltonian cycle on 12 nodes has lhs >= 3k + 1 (random sample).
    std::mt19937_64 rng(7);
    std::vector<uint32_t> tour(p.n);
    for (uint32_t i = 0; i < p.n; ++i) tour[i] = i;
    for (int s = 0; s < 3000; ++s) {
        std::shuffle(tour.begin(), tour.end(), rng);
        for (const auto& cut : cuts) CHECK(tour_lhs(tour, cut, p.n) >= 10.0 - 1e-9);
    }

    // Pool row: same violation on the LP columns, survives save / load, dual sets = handle + teeth.
    apex::CutPool pool;
    pool.stage_comb(c);
    const auto added = pool.commit();
    REQUIRE(added.size() == 1);
    CHECK(pool.violation(pool.at(added[0]), p.edges, p.x) == doctest::Approx(0.4));
    const auto row = pool.make_row(pool.at(added[0]), p.edges);
    CHECK(row.rhs == 10.0);
    size_t sets = 0;
    pool.at(added[0]).for_each_set([&](const std::vector<uint32_t>&) { ++sets; });
    CHECK(sets == 4);
    std::stringstream ss;
    pool.save(ss);
    apex::CutPool back;
    back.load(ss);
    REQUIRE(back.size() == 1);
    CHECK(back.at(0).sets == pool.at(0).sets);
    CHECK(back.violation(back.at(0), p.edges, p.x) == doctest::Approx(0.4));
}
