#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <random>
#include <sstream>
#include <vector>

#include "cuts/comb.hpp"
#include "cuts/domino.hpp"
#include "cuts/pool.hpp"

using apex::Domino;
using apex::Edge;

namespace {

apex::SupportGraph tour_support(const std::vector<uint32_t>& tour, uint32_t n) {
    std::vector<Edge> e;
    std::vector<double> x;
    for (size_t i = 0; i < tour.size(); ++i) {
        const uint32_t a = tour[i], b = tour[(i + 1) % tour.size()];
        e.push_back(Edge{std::min(a, b), std::max(a, b), 1, apex::kActive, 0.0});
        x.push_back(1.0);
    }
    return apex::build_support(n, e, x);
}

// Planar comb gadget: inner hexagon a1 a2 b1 b2 c1 c2 (radius 1), outer hexagon (radius 3),
// chain edges g1-g2 x = 1, hexagon edges between gadget vertices x = 0.6, radial spokes x = 0.4.
// The comb (handle = inner hexagon, teeth {g1, g2, g'1, g'2}) is violated by 0.4.
struct Planar {
    uint32_t n = 12;
    std::vector<double> xs, ys;
    std::vector<Edge> edges;
    std::vector<double> x;
};

Planar planar_gadget() {
    Planar p;
    p.xs.resize(12);
    p.ys.resize(12);
    for (uint32_t k = 0; k < 6; ++k) {  // inner k, outer k + 6, same angle
        const double th = 2 * M_PI * k / 6 + 0.1;
        p.xs[k] = std::cos(th);
        p.ys[k] = std::sin(th);
        p.xs[k + 6] = 3 * std::cos(th);
        p.ys[k + 6] = 3 * std::sin(th);
    }
    std::map<std::pair<uint32_t, uint32_t>, double> acc;
    auto add = [&](uint32_t a, uint32_t b, double w) { acc[{std::min(a, b), std::max(a, b)}] += w; };
    for (uint32_t base : {0u, 6u})
        for (uint32_t k = 0; k < 6; ++k) add(base + k, base + (k + 1) % 6, k % 2 == 0 ? 1.0 : 0.6);
    for (uint32_t k = 0; k < 6; ++k) add(k, k + 6, 0.4);
    for (const auto& [k, w] : acc) {
        p.edges.push_back(Edge{k.first, k.second, 1, apex::kActive, 0.0});
        p.x.push_back(w);
    }
    return p;
}

}  // namespace

TEST_CASE("T_dp_valid_structure") {
    using V = std::vector<uint32_t>;
    CHECK(apex::dp_valid(8, {Domino{V{0}, V{1}}}));
    CHECK_FALSE(apex::dp_valid(8, {Domino{V{0}, V{1}}, Domino{V{2}, V{3}}}));                // k even
    CHECK_FALSE(apex::dp_valid(8, {Domino{V{0}, V{0, 1}}}));                                 // A, B overlap
    CHECK_FALSE(apex::dp_valid(8, {Domino{V{}, V{1}}}));                                     // empty side
    CHECK_FALSE(apex::dp_valid(4, {Domino{V{0, 1}, V{2, 3}}}));                              // T = V
    CHECK(apex::dp_valid(8, {Domino{V{0}, V{1}}, Domino{V{1}, V{2}}, Domino{V{0, 3}, V{4}}}));  // dominoes may overlap
}

// The validity argument in cuts/domino.hpp, checked by brute force: random dominoes (k odd,
// overlapping allowed) and random S on 8 nodes, against all 2520 tours.
TEST_CASE("T_dp_valid_for_every_tour") {
    const uint32_t n = 8;
    std::vector<std::vector<uint32_t>> tours;
    std::vector<uint32_t> perm(n);
    std::iota(perm.begin(), perm.end(), 0);
    do {
        if (perm[1] < perm[n - 1]) tours.push_back(perm);  // each cycle once (start 0, one direction)
    } while (std::next_permutation(perm.begin() + 1, perm.end()));
    REQUIRE(tours.size() == 2520);
    std::vector<apex::SupportGraph> sup;
    for (const auto& t : tours) sup.push_back(tour_support(t, n));
    std::mt19937_64 rng(11);
    int tight = 0;
    for (int trial = 0; trial < 300; ++trial) {
        const uint32_t k = 1 + 2 * static_cast<uint32_t>(rng() % 3);
        std::vector<Domino> dom;
        while (dom.size() < k) {
            Domino d;
            for (uint32_t v = 0; v < n; ++v) {
                const auto r = rng() % 4;
                if (r == 0) d.a.push_back(v);
                else if (r == 1) d.b.push_back(v);
            }
            if (!d.a.empty() && !d.b.empty() && d.a.size() + d.b.size() < n) dom.push_back(d);
        }
        std::vector<uint32_t> S;
        for (uint32_t v = 0; v < n; ++v)
            if (rng() % 2) S.push_back(v);
        REQUIRE(apex::dp_valid(n, dom));
        double best = 1e9;
        for (const auto& g : sup) best = std::min(best, -apex::dp_violation(g, S, dom));  // lhs - (3k + 1)
        CHECK(best >= -1e-9);
        tight += best < 1e-9;
    }
    CHECK(tight > 0);  // the bound is attained for some random choices (the test is not vacuous)
}

TEST_CASE("T_dp_contains_combs") {
    const auto p = planar_gadget();
    const auto g = apex::build_support(p.n, p.edges, p.x);
    using V = std::vector<uint32_t>;
    const V H{0, 1, 2, 3, 4, 5};
    const std::vector<V> teeth{{0, 1, 6, 7}, {2, 3, 8, 9}, {4, 5, 10, 11}};
    CHECK(apex::comb_violation(g, H, teeth) == doctest::Approx(0.4));
    std::vector<Domino> dom;
    for (const auto& t : teeth) dom.push_back({V{t[0], t[1]}, V{t[2], t[3]}});
    CHECK(apex::dp_violation(g, H, dom) == doctest::Approx(0.4));
}

TEST_CASE("T_dp_separates_planar_gadget") {
    const auto p = planar_gadget();
    const auto g = apex::build_support(p.n, p.edges, p.x);
    apex::DpStats st;
    const auto cuts = apex::separate_dp(g, p.xs, p.ys, {}, &st);
    CHECK(st.dropped_edges == 0);
    REQUIRE(!cuts.empty());
    CHECK(cuts[0].violation >= 0.4 - 1e-9);
    for (const auto& c : cuts) {
        CHECK(apex::dp_valid(p.n, c.dominoes));
        CHECK(c.violation == doctest::Approx(apex::dp_violation(g, c.handle, c.dominoes)));
    }
    // Pool row: exact on LP columns, save / load, pricing sets = S + every A_j, B_j.
    apex::CutPool pool;
    pool.stage_dp(cuts[0]);
    const auto added = pool.commit();
    REQUIRE(added.size() == 1);
    const auto& pc = pool.at(added[0]);
    CHECK(pool.violation(pc, p.edges, p.x) == doctest::Approx(cuts[0].violation));
    size_t sets = 0;
    pc.for_each_set([&](const std::vector<uint32_t>&) { ++sets; });
    CHECK(sets == 1 + 2 * cuts[0].dominoes.size());
    // Pricing bound: sum of crossings >= the row coefficient on every pair of nodes.
    const auto row_all = [&]() {
        std::vector<Edge> all;
        for (uint32_t u = 0; u < p.n; ++u)
            for (uint32_t v = u + 1; v < p.n; ++v) all.push_back(Edge{u, v, 1, apex::kActive, 0.0});
        return all;
    }();
    const auto row = pool.make_row(pc, row_all);
    std::vector<double> coef(row_all.size(), 0.0);
    for (const auto& e : row.entries) coef[e.index] = e.value;
    for (size_t j = 0; j < row_all.size(); ++j) {
        int crossings = 0;
        pc.for_each_set([&](const std::vector<uint32_t>& s) {
            const bool a = std::binary_search(s.begin(), s.end(), row_all[j].u);
            const bool b = std::binary_search(s.begin(), s.end(), row_all[j].v);
            crossings += a != b;
        });
        CHECK(coef[j] <= crossings);
    }
    std::stringstream ss;
    pool.save(ss);
    apex::CutPool back;
    back.load(ss);
    REQUIRE(back.size() == 1);
    CHECK(back.violation(back.at(0), p.edges, p.x) == doctest::Approx(cuts[0].violation));
}
