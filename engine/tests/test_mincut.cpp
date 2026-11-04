#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <vector>

#include "cuts/mincut.hpp"

using apex::Edge;
using apex::SupportGraph;

namespace {

struct WEdge { uint32_t u, v; double w; };

SupportGraph graph(uint32_t n, const std::vector<WEdge>& es) {
    std::vector<Edge> edges;
    std::vector<double> x;
    for (const auto& e : es) {
        edges.push_back(Edge{std::min(e.u, e.v), std::max(e.u, e.v), 1, apex::kActive, 0.0});
        x.push_back(e.w);
    }
    return apex::build_support(n, edges, x, 0.0);
}

double brute_global(const SupportGraph& g) {
    double best = INFINITY;
    for (uint32_t mask = 1; mask < (1u << g.n) - 1; ++mask) {
        if (!(mask & 1u)) continue;
        double c = 0;
        for (const auto& e : g.edges)
            if (((mask >> e.u) & 1u) != ((mask >> e.v) & 1u)) c += e.cap;
        best = std::min(best, c);
    }
    return best;
}

double brute_st(const SupportGraph& g, uint32_t s, uint32_t t) {
    double best = INFINITY;
    for (uint32_t mask = 0; mask < (1u << g.n); ++mask) {
        if (!((mask >> s) & 1u) || ((mask >> t) & 1u)) continue;
        double c = 0;
        for (const auto& e : g.edges)
            if (((mask >> e.u) & 1u) != ((mask >> e.v) & 1u)) c += e.cap;
        best = std::min(best, c);
    }
    return best;
}

}  // namespace

TEST_CASE("T_stoer_wagner_known_cuts") {
    struct Case { const char* name; uint32_t n; std::vector<WEdge> e; double expected; };
    const std::vector<Case> cases = {
        // Stoer & Wagner (1997), Fig. 1: min cut {3,4,7,8} | {1,2,5,6} (0-based here) = 3 + 1 = 4.
        {"stoer_wagner_paper", 8,
         {{0, 1, 2}, {0, 4, 3}, {1, 2, 3}, {1, 4, 2}, {1, 5, 2}, {2, 3, 4}, {2, 6, 2}, {3, 6, 2}, {3, 7, 2},
          {4, 5, 3}, {5, 6, 1}, {6, 7, 3}},
         4},
        {"single_edge", 2, {{0, 1, 2.5}}, 2.5},
        {"path_4", 4, {{0, 1, 3}, {1, 2, 1}, {2, 3, 5}}, 1},              // weakest edge
        {"cycle_5_unit", 5, {{0, 1, 1}, {1, 2, 1}, {2, 3, 1}, {3, 4, 1}, {4, 0, 1}}, 2},
        {"cycle_weighted", 4, {{0, 1, 5}, {1, 2, 1}, {2, 3, 5}, {3, 0, 2}}, 3},  // cut {1,2}|{3,0}: 1+2
        {"star_4", 5, {{0, 1, 2}, {0, 2, 3}, {0, 3, 1.5}, {0, 4, 4}}, 1.5},  // isolate leaf 3
        {"K4_unit", 4, {{0, 1, 1}, {0, 2, 1}, {0, 3, 1}, {1, 2, 1}, {1, 3, 1}, {2, 3, 1}}, 3},
        {"K5_unit", 5,
         {{0, 1, 1}, {0, 2, 1}, {0, 3, 1}, {0, 4, 1}, {1, 2, 1}, {1, 3, 1}, {1, 4, 1}, {2, 3, 1}, {2, 4, 1}, {3, 4, 1}},
         4},
        {"two_triangles_bridge", 6,
         {{0, 1, 1}, {1, 2, 1}, {0, 2, 1}, {3, 4, 1}, {4, 5, 1}, {3, 5, 1}, {2, 3, 0.5}}, 0.5},
        {"disconnected", 4, {{0, 1, 1}, {2, 3, 1}}, 0},
        {"grid_3x3_unit", 9,
         {{0, 1, 1}, {1, 2, 1}, {3, 4, 1}, {4, 5, 1}, {6, 7, 1}, {7, 8, 1}, {0, 3, 1}, {3, 6, 1}, {1, 4, 1}, {4, 7, 1},
          {2, 5, 1}, {5, 8, 1}},
         2},  // corner node has degree 2
        {"subtour_half", 6,  // 0.5 * (tour 0..5) + 0.5 * (triangles {0,1,2},{3,4,5}): cut 0.5 + 0.5
         {{0, 1, 1}, {1, 2, 1}, {2, 3, 0.5}, {3, 4, 1}, {4, 5, 1}, {5, 0, 0.5}, {0, 2, 0.5}, {3, 5, 0.5}},
         1},
    };
    for (const auto& c : cases) {
        CAPTURE(c.name);
        const auto g = graph(c.n, c.e);
        apex::StoerWagner sw(g);
        const auto r = sw.run();
        CHECK(r.value == doctest::Approx(c.expected));
        CHECK(brute_global(g) == doctest::Approx(c.expected));  // hand value double-checked
        if (c.expected > 0) CHECK(apex::cut_value(g, r.side) == doctest::Approx(r.value));
    }
    // Random graphs vs brute force.
    std::mt19937_64 rng(3);
    std::uniform_real_distribution<double> U(0.1, 2.0);
    int mism = 0;
    for (int t = 0; t < 200; ++t) {
        const uint32_t n = 2 + rng() % 11;
        std::vector<WEdge> es;
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t j = i + 1; j < n; ++j)
                if (rng() % 3 == 0) es.push_back({i, j, std::round(U(rng) * 4) / 4});
        const auto g = graph(n, es);
        apex::StoerWagner sw(g);
        const auto r = sw.run();
        mism += std::abs(r.value - brute_global(g)) > 1e-9;
        if (r.side.size() > 0 && r.side.size() < n) mism += std::abs(apex::cut_value(g, r.side) - r.value) > 1e-9;
    }
    CHECK(mism == 0);
}

TEST_CASE("T_dinic_known_flows") {
    // CLRS Fig. 26.1 network (directed): max flow 23.
    apex::Dinic d(6);
    d.add_arc(0, 1, 16); d.add_arc(0, 2, 13); d.add_arc(1, 3, 12); d.add_arc(2, 1, 4); d.add_arc(2, 4, 14);
    d.add_arc(3, 2, 9);  d.add_arc(3, 5, 20); d.add_arc(4, 3, 7);  d.add_arc(4, 5, 4);
    CHECK(d.max_flow(0, 5) == doctest::Approx(23));
    const auto cut = d.min_cut_between(0, 5);
    CHECK(cut.value == doctest::Approx(23));
    CHECK(cut.side == std::vector<uint32_t>{0, 1, 2, 4});  // min cut {s,v1,v2,v4}: 12 + 7 + 4
    // Undirected random graphs vs brute-force s-t cuts.
    std::mt19937_64 rng(4);
    std::uniform_real_distribution<double> U(0.1, 2.0);
    int mism = 0;
    for (int t = 0; t < 200; ++t) {
        const uint32_t n = 2 + rng() % 10;
        std::vector<WEdge> es;
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t j = i + 1; j < n; ++j)
                if (rng() % 2 == 0) es.push_back({i, j, std::round(U(rng) * 4) / 4});
        const auto g = graph(n, es);
        apex::Dinic dn(g);
        const uint32_t s = rng() % n, tt = (s + 1 + rng() % (n - 1)) % n;
        const auto r = dn.min_cut_between(s, tt);
        mism += std::abs(r.value - brute_st(g, s, tt)) > 1e-9;
        mism += std::abs(apex::cut_value(g, r.side) - r.value) > 1e-9;
        mism += !std::binary_search(r.side.begin(), r.side.end(), s) || std::binary_search(r.side.begin(), r.side.end(), tt);
    }
    CHECK(mism == 0);
}

TEST_CASE("T_sec_separation_finds_planted") {
    // n = 100, connected fractional point: 0.5 * Hamiltonian tour + 0.5 * 2-factor that contains a
    // planted subtour S (|S| = 30): x(delta(S)) = 0.5 * 2 = 1 < 2. Must be found within 3 rounds
    // (one call here is one round; no LP between rounds).
    std::mt19937_64 rng(77);
    const uint32_t n = 100;
    for (int trial = 0; trial < 5; ++trial) {
        auto shuffled = [&](std::vector<uint32_t> v) {
            for (size_t i = v.size() - 1; i > 0; --i) std::swap(v[i], v[rng() % (i + 1)]);
            return v;
        };
        std::vector<uint32_t> all(n);
        for (uint32_t i = 0; i < n; ++i) all[i] = i;
        const auto perm2 = shuffled(all);  // S = perm2[0..29]
        const auto s_order = shuffled(std::vector<uint32_t>(perm2.begin(), perm2.begin() + 30));
        const auto r_order = shuffled(std::vector<uint32_t>(perm2.begin() + 30, perm2.end()));
        std::vector<uint32_t> perm(s_order);  // tour visits S as one block: crosses delta(S) twice
        perm.insert(perm.end(), r_order.begin(), r_order.end());
        std::map<std::pair<uint32_t, uint32_t>, double> acc;
        auto add = [&](uint32_t a, uint32_t b, double w) { acc[{std::min(a, b), std::max(a, b)}] += w; };
        for (uint32_t k = 0; k < n; ++k) add(perm[k], perm[(k + 1) % n], 0.5);  // tour
        const auto s2 = shuffled(s_order), r2 = shuffled(r_order);               // 2-factor: two subtours
        for (uint32_t k = 0; k < 30; ++k) add(s2[k], s2[(k + 1) % 30], 0.5);
        for (uint32_t k = 0; k < 70; ++k) add(r2[k], r2[(k + 1) % 70], 0.5);
        std::vector<Edge> edges;
        std::vector<double> x;
        for (const auto& [key, w] : acc) { edges.push_back(Edge{key.first, key.second, 1, apex::kActive, 0.0}); x.push_back(w); }
        const auto g = apex::build_support(n, edges, x);
        REQUIRE(apex::connected_components(g).size() == 1);
        std::vector<uint32_t> planted(perm2.begin(), perm2.begin() + 30);
        std::sort(planted.begin(), planted.end());
        REQUIRE(apex::cut_value(g, planted) == doctest::Approx(1.0));

        bool found = false;
        int rounds = 0;
        for (; rounds < 3 && !found; ++rounds) {
            apex::SecSeparationStats st;
            const auto cuts = apex::separate_sec(g, {64, 1e-5, static_cast<uint64_t>(rounds + 1)}, &st);
            for (const auto& c : cuts) {
                CHECK(apex::cut_value(g, c.nodes) < 2 - 1e-5);  // every emitted cut is violated
                found |= c.nodes == planted;
            }
            if (trial == 0) MESSAGE("cuts=" << cuts.size() << " sw=" << st.stoer_wagner_cuts << " pairwise=" << st.pairwise_cuts << " shrunk_nodes=" << st.shrunk_nodes);
        }
        CHECK(found);
        CHECK(rounds <= 3);
    }
}

TEST_CASE("T_sec_separation_no_false_cuts") {
    // A convex combination of Hamiltonian tours satisfies every SEC: nothing may be emitted.
    std::mt19937_64 rng(5);
    const uint32_t n = 60;
    std::map<std::pair<uint32_t, uint32_t>, double> acc;
    for (int t = 0; t < 4; ++t) {
        std::vector<uint32_t> perm(n);
        for (uint32_t i = 0; i < n; ++i) perm[i] = i;
        for (uint32_t i = n - 1; i > 0; --i) std::swap(perm[i], perm[rng() % (i + 1)]);
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t a = perm[k], b = perm[(k + 1) % n];
            acc[{std::min(a, b), std::max(a, b)}] += 0.25;
        }
    }
    std::vector<Edge> edges;
    std::vector<double> x;
    for (const auto& [key, w] : acc) { edges.push_back(Edge{key.first, key.second, 1, apex::kActive, 0.0}); x.push_back(w); }
    CHECK(apex::separate_sec(apex::build_support(n, edges, x)).empty());
}
