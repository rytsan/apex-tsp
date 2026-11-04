#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <vector>

#include "cuts/shrink.hpp"

using apex::Edge;

namespace {

// Random fractional point satisfying x(delta(v)) = 2 exactly: convex combination (weights in
// multiples of 1/4, exact in binary) of random Hamiltonian cycles and random 2-factors made of
// several subtours (which violate SECs). Common edges accumulate to x = 1.
struct Point {
    std::vector<Edge> edges;
    std::vector<double> x;
};

Point random_point(std::mt19937_64& rng, uint32_t n) {
    std::map<std::pair<uint32_t, uint32_t>, double> acc;
    const int parts = 2 + static_cast<int>(rng() % 3);  // 2..4 structures
    std::vector<int> quarters(parts, 1);
    for (int left = 4 - parts; left > 0; --left) ++quarters[rng() % parts];
    for (int p = 0; p < parts; ++p) {
        std::vector<uint32_t> perm(n);
        for (uint32_t i = 0; i < n; ++i) perm[i] = i;
        for (uint32_t i = n - 1; i > 0; --i) std::swap(perm[i], perm[rng() % (i + 1)]);
        // Split into cycles of length >= 3 (one cycle = Hamiltonian tour).
        std::vector<uint32_t> cuts{0};
        if ((p == 0 || rng() % 2) && n >= 6) {  // part 0 always has subtours
            uint32_t pos = 3 + static_cast<uint32_t>(rng() % (n - 5));
            cuts.push_back(pos);
            if (n - pos >= 6 && rng() % 2) cuts.push_back(pos + 3 + static_cast<uint32_t>(rng() % (n - pos - 5)));
        }
        cuts.push_back(n);
        for (size_t c = 0; c + 1 < cuts.size(); ++c)
            for (uint32_t k = cuts[c]; k < cuts[c + 1]; ++k) {
                const uint32_t a = perm[k], b = perm[k + 1 == cuts[c + 1] ? cuts[c] : k + 1];
                acc[{std::min(a, b), std::max(a, b)}] += 0.25 * quarters[p];
            }
    }
    Point pt;
    for (const auto& [key, v] : acc) {
        pt.edges.push_back(Edge{key.first, key.second, 1, apex::kActive, 0.0});
        pt.x.push_back(v);
    }
    return pt;
}

// Brute-force min over proper nonempty S of x(delta(S)); returns value and one argmin mask.
double brute_min_cut(const apex::SupportGraph& g, uint32_t* best_mask = nullptr) {
    double best = INFINITY;
    for (uint32_t mask = 1; mask < (1u << g.n) - 1; ++mask) {
        if (!(mask & 1u)) continue;  // S and V\S give the same cut: fix node 0 in S
        double c = 0;
        for (const auto& e : g.edges)
            if (((mask >> e.u) & 1u) != ((mask >> e.v) & 1u)) c += e.cap;
        if (c < best) { best = c; if (best_mask) *best_mask = mask; }
    }
    return best;
}

}  // namespace

// Padberg & Rinaldi (1990), "Facet identification for the symmetric traveling salesman
// polytope", Math. Programming 47: shrinking x_e = 1 edges preserves the minimum cut.
// Property test on >= 100 random fractional points with n <= 14, all SECs enumerated.
TEST_CASE("T_shrinking_safe") {
    std::mt19937_64 rng(2026);
    int points = 0, with_violation = 0, shrunk_some = 0;
    while (points < 150) {
        const uint32_t n = 6 + static_cast<uint32_t>(rng() % 9);  // 6..14
        const auto pt = random_point(rng, n);
        const auto g = apex::build_support(n, pt.edges, pt.x);
        const auto s = apex::shrink_unit_edges(g);
        ++points;
        shrunk_some += s.g.n < n;
        const double orig = brute_min_cut(g);
        with_violation += orig < 2 - 1e-5;
        if (s.g.n < 2) {  // everything contracted: only possible when no SEC is violated
            CHECK(orig >= 2 - 1e-9);
            continue;
        }
        uint32_t mask = 0;
        const double shr = brute_min_cut(s.g, &mask);
        CHECK(shr == doctest::Approx(orig).epsilon(1e-12));  // min cut preserved exactly
        // The minimizing shrunken cut expands to an original cut of the same value.
        std::vector<uint32_t> sup;
        for (uint32_t k = 0; k < s.g.n; ++k)
            if ((mask >> k) & 1u) sup.push_back(k);
        CHECK(apex::cut_value(g, s.expand(sup)) == doctest::Approx(shr));
        // Supernode degree is 2, or < 2 (a violated SEC kept visible as a singleton) and equal
        // to the cut of its expansion in the original graph.
        for (uint32_t k = 0; k < s.g.n; ++k) {
            const double d = apex::cut_value(s.g, {k});
            CHECK(d <= 2.0 + 1e-9);
            CHECK(apex::cut_value(g, s.expand({k})) == doctest::Approx(d));
        }
    }
    MESSAGE("points=" << points << " with_violated_SEC=" << with_violation << " shrunk=" << shrunk_some);
    CHECK(with_violation > 20);
    CHECK(shrunk_some > 20);
}

TEST_CASE("T_shrinking_expands_correctly") {
    // Path 0-1-2 (x=1), 3-4 (x=1), and fractional edges: supernodes {0,1,2}, {3,4}, {5}, {6}.
    const std::vector<Edge> edges = {{0, 1, 1, 4, 0}, {1, 2, 1, 4, 0}, {3, 4, 1, 4, 0}, {2, 5, 1, 4, 0},
                                     {0, 6, 1, 4, 0}, {2, 3, 1, 4, 0}, {4, 5, 1, 4, 0}, {5, 6, 1, 4, 0},
                                     {6, 0, 1, 4, 0}, {3, 6, 1, 4, 0}};
    const std::vector<double> x = {1, 1, 1, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5};
    const auto g = apex::build_support(7, edges, x);
    const auto s = apex::shrink_unit_edges(g);
    REQUIRE(s.members.size() == s.g.n);
    std::vector<uint32_t> all;
    for (uint32_t k = 0; k < s.g.n; ++k) {
        const auto e = s.expand({k});
        CHECK(std::is_sorted(e.begin(), e.end()));
        all.insert(all.end(), e.begin(), e.end());
        for (uint32_t v : e) CHECK(s.super_of[v] == k);
    }
    std::sort(all.begin(), all.end());
    CHECK(all == std::vector<uint32_t>{0, 1, 2, 3, 4, 5, 6});  // partition of V
    // {0,1,2} contract (x = 1 path); node 6 also joins: edges (0,6) and (6,0) sum to x = 1.
    const auto& m0 = s.members[s.super_of[0]];
    for (uint32_t v : {0u, 1u, 2u, 6u}) CHECK(std::binary_search(m0.begin(), m0.end(), v));
    for (uint32_t a = 0; a < s.g.n; ++a)
        for (uint32_t b = a + 1; b < s.g.n; ++b)
            CHECK(apex::cut_value(g, s.expand({a, b})) == doctest::Approx(apex::cut_value(s.g, {a, b})));
    // A Hamiltonian cycle with x = 1 everywhere shrinks to one supernode.
    std::vector<Edge> ring;
    const uint32_t n = 100000;
    for (uint32_t i = 0; i < n; ++i) ring.push_back({std::min(i, (i + 1) % n), std::max(i, (i + 1) % n), 1, 4, 0});
    const auto r = apex::shrink_unit_edges(apex::build_support(n, ring, std::vector<double>(n, 1.0)));
    CHECK(r.g.n == 1);
    CHECK(r.g.edges.empty());
}
