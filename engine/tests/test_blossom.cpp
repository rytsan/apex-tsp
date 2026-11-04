#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <vector>

#include "cuts/blossom.hpp"

using apex::BlossomConfig;
using apex::BlossomMode;
using apex::Edge;

namespace {

struct Point {
    uint32_t n;
    std::vector<Edge> edges;
    std::vector<double> x;
};

Point from_map(uint32_t n, const std::map<std::pair<uint32_t, uint32_t>, double>& acc) {
    Point p{n, {}, {}};
    for (const auto& [k, w] : acc) {
        p.edges.push_back(Edge{k.first, k.second, 1, apex::kActive, 0.0});
        p.x.push_back(w);
    }
    return p;
}

// Classic violated blossom gadget: handle triangle {a,b,c} with x = 1/2 inside, teeth (a,a'),
// (b,b'), (c,c') with x = 1, outer triangle {a',b',c'} with x = 1/2. Degrees are 2, every SEC
// holds (x(delta(H)) = 3), blossom H with 3 teeth is violated by exactly 1.
void add_gadget(std::map<std::pair<uint32_t, uint32_t>, double>& acc, const uint32_t v[6]) {
    auto add = [&](uint32_t a, uint32_t b, double w) { acc[{std::min(a, b), std::max(a, b)}] += w; };
    add(v[0], v[1], 0.5); add(v[1], v[2], 0.5); add(v[0], v[2], 0.5);
    add(v[3], v[4], 0.5); add(v[4], v[5], 0.5); add(v[3], v[5], 0.5);
    add(v[0], v[3], 1); add(v[1], v[4], 1); add(v[2], v[5], 1);
}

// Random degree-2 fractional point: convex combination (quarters) of tours / 2-factors /
// blossom gadget structures.
Point random_point(std::mt19937_64& rng, uint32_t n) {
    std::map<std::pair<uint32_t, uint32_t>, double> acc;
    auto add = [&](uint32_t a, uint32_t b, double w) { acc[{std::min(a, b), std::max(a, b)}] += w; };
    for (int q = 0; q < 4; ++q) {
        std::vector<uint32_t> perm(n);
        for (uint32_t i = 0; i < n; ++i) perm[i] = i;
        for (uint32_t i = n - 1; i > 0; --i) std::swap(perm[i], perm[rng() % (i + 1)]);
        const uint32_t kind = rng() % 3;
        if (kind == 0 || n < 9) {
            for (uint32_t k = 0; k < n; ++k) add(perm[k], perm[(k + 1) % n], 0.25);  // tour
        } else if (kind == 1) {
            const uint32_t cut = 3 + rng() % (n - 5);  // two subtours
            for (uint32_t k = 0; k < cut; ++k) add(perm[k], perm[(k + 1) % cut], 0.25);
            for (uint32_t k = cut; k < n; ++k) add(perm[k], perm[k + 1 == n ? cut : k + 1], 0.25);
        } else {  // gadget on 6 nodes (as 2 quarter-weighted 2-factors) + tour on the rest
            std::map<std::pair<uint32_t, uint32_t>, double> gad;
            add_gadget(gad, perm.data());
            for (const auto& [k, w] : gad) acc[k] += 0.25 * w;
            for (uint32_t k = 6; k < n; ++k) add(perm[k], perm[k + 1 == n ? 6 : k + 1], 0.25);
        }
    }
    return from_map(n, acc);
}

// Half-integral 2-matching vertex (the structures blossoms cut off): an even number of odd
// cycles with x = 1/2, their nodes paired across cycles by x = 1 edges (spare nodes spliced into
// those edges as x = 1 paths); remaining nodes on an integral cycle. Optionally mixed 3:1 with a
// random tour. Degrees are exactly 2.
Point half_integral(std::mt19937_64& rng, uint32_t n, bool mix_tour) {
    for (;;) {
        std::vector<uint32_t> perm(n);
        for (uint32_t i = 0; i < n; ++i) perm[i] = i;
        for (uint32_t i = n - 1; i > 0; --i) std::swap(perm[i], perm[rng() % (i + 1)]);
        std::map<std::pair<uint32_t, uint32_t>, double> acc;
        auto add = [&](uint32_t a, uint32_t b, double w) { acc[{std::min(a, b), std::max(a, b)}] += w; };
        uint32_t used = 0;
        std::vector<std::pair<uint32_t, uint32_t>> cyc_nodes;  // (node, cycle id)
        const uint32_t ncyc = 2 * (1 + static_cast<uint32_t>(rng() % 2));
        bool ok = true;
        for (uint32_t c = 0; c < ncyc; ++c) {
            const uint32_t len = rng() % 2 ? 3 : 5;
            if (used + len > n) { ok = false; break; }
            for (uint32_t k = 0; k < len; ++k) {
                add(perm[used + k], perm[used + (k + 1) % len], 0.5);
                cyc_nodes.push_back({perm[used + k], c});
            }
            used += len;
        }
        if (!ok) continue;
        // Pair cycle nodes across different cycles.
        for (size_t i = cyc_nodes.size() - 1; i > 0; --i) std::swap(cyc_nodes[i], cyc_nodes[rng() % (i + 1)]);
        std::vector<std::pair<uint32_t, uint32_t>> match;
        for (size_t i = 0; i + 1 < cyc_nodes.size(); i += 2) {
            if (cyc_nodes[i].second == cyc_nodes[i + 1].second) { ok = false; break; }
            match.push_back({cyc_nodes[i].first, cyc_nodes[i + 1].first});
        }
        if (!ok) continue;
        const uint32_t spare = n - used;
        uint32_t ring = spare >= 3 ? spare : 0, splice = spare - ring;
        if (ring > 0 && rng() % 2 && spare >= 4) { ring = 0; splice = spare; }  // sometimes splice all
        uint32_t next = used;
        for (size_t mi = 0; mi < match.size(); ++mi) {
            uint32_t a = match[mi].first;
            const uint32_t take = mi + 1 == match.size() ? splice : std::min<uint32_t>(splice, rng() % 3);
            for (uint32_t k = 0; k < take; ++k) { add(a, perm[next], 1); a = perm[next++]; }
            splice -= take;
            add(a, match[mi].second, 1);
        }
        for (uint32_t k = 0; k < ring; ++k) add(perm[next + k], perm[next + (k + 1) % ring], 1);
        if (mix_tour) {
            for (auto& [k, w] : acc) w *= 0.75;
            std::vector<uint32_t> t(n);
            for (uint32_t i = 0; i < n; ++i) t[i] = i;
            for (uint32_t i = n - 1; i > 0; --i) std::swap(t[i], t[rng() % (i + 1)]);
            for (uint32_t k = 0; k < n; ++k) add(t[k], t[(k + 1) % n], 0.25);
        }
        return from_map(n, acc);
    }
}

// Exact minimum over all handles (2^n) with optimal teeth: reference for Padberg-Rao.
double brute_best_violation(const apex::SupportGraph& g) {
    double best = -INFINITY;
    for (uint32_t mask = 1; mask < (1u << g.n) - 1; ++mask) {
        std::vector<uint32_t> h;
        for (uint32_t v = 0; v < g.n; ++v)
            if ((mask >> v) & 1u) h.push_back(v);
        std::vector<apex::BlossomTooth> t;
        best = std::max(best, apex::best_teeth(g, h, t));
    }
    return best;
}

// The inequality must hold for any Hamiltonian cycle (validity), using endpoints only.
bool satisfied_by_tour(const apex::BlossomCut& c, const std::vector<uint32_t>& tour, uint32_t n) {
    std::vector<uint8_t> in(n, 0);
    for (uint32_t v : c.handle) in[v] = 1;
    double lhs = 0;
    for (size_t k = 0; k < tour.size(); ++k) {
        const uint32_t a = tour[k], b = tour[(k + 1) % tour.size()];
        if (in[a] == in[b]) continue;
        const bool tooth = std::any_of(c.teeth.begin(), c.teeth.end(), [&](const apex::BlossomTooth& t) {
            return t.u == std::min(a, b) && t.v == std::max(a, b);
        });
        lhs += tooth ? -1 : 1;
    }
    return lhs >= 1.0 - static_cast<double>(c.teeth.size()) - 1e-12;
}

}  // namespace

TEST_CASE("T_blossom_spec_formula_is_invalid_for_tsp") {
    // Documents the MASTER §6 correction: x(E(S)) <= (|S|-1)/2 is violated by a plain tour
    // (S = 3 consecutive nodes: x(E(S)) = 2 > 1), so it must never be used as a TSP cut.
    const double xES = 2.0, rhs = (3.0 - 1.0) / 2.0;
    CHECK(xES > rhs);
}

TEST_CASE("T_blossom_planted_violation") {
    std::mt19937_64 rng(9);
    for (uint32_t n : {6u, 20u, 60u}) {
        CAPTURE(n);
        std::vector<uint32_t> perm(n);
        for (uint32_t i = 0; i < n; ++i) perm[i] = i;
        for (uint32_t i = n - 1; i > 0; --i) std::swap(perm[i], perm[rng() % (i + 1)]);
        std::map<std::pair<uint32_t, uint32_t>, double> acc;
        add_gadget(acc, perm.data());
        for (uint32_t k = 6; k < n; ++k)  // integral cycle through the remaining nodes
            acc[{std::min(perm[k], perm[k + 1 == n ? 6 : k + 1]), std::max(perm[k], perm[k + 1 == n ? 6 : k + 1])}] += 1;
        const auto p = from_map(n, acc);
        const auto g = apex::build_support(n, p.edges, p.x);
        std::vector<uint32_t> handle(perm.begin(), perm.begin() + 3);
        std::sort(handle.begin(), handle.end());
        for (auto mode : {BlossomMode::Heuristic, BlossomMode::Full}) {
            BlossomConfig cfg;
            cfg.mode = mode;
            apex::BlossomStats st;
            const auto cuts = apex::separate_blossom(g, cfg, &st);
            bool found = false;
            for (const auto& c : cuts) {
                CHECK(c.teeth.size() % 2 == 1);
                CHECK(apex::blossom_violation(g, c.handle, c.teeth) == doctest::Approx(c.violation));
                std::vector<uint32_t> outer(perm.begin() + 3, perm.begin() + 6);
                std::sort(outer.begin(), outer.end());
                found |= (c.handle == handle || c.handle == outer) && c.teeth.size() == 3 &&
                         c.violation == doctest::Approx(1.0);
            }
            CHECK(found);
            CHECK(st.heuristic == (mode == BlossomMode::Heuristic));
        }
    }
}

TEST_CASE("T_blossom_padberg_rao_exact") {
    // Full mode finds the maximally violated blossom (brute force over all handles, n <= 10).
    std::mt19937_64 rng(21);
    int with_violation = 0;
    for (int trial = 0; trial < 60; ++trial) {
        const uint32_t n = 10 + static_cast<uint32_t>(rng() % 5);
        const auto p = trial % 3 == 0 ? random_point(rng, n) : half_integral(rng, n, trial % 3 == 2);
        const auto g = apex::build_support(n, p.edges, p.x);
        const double best = brute_best_violation(g);
        BlossomConfig cfg;
        cfg.mode = BlossomMode::Full;
        const auto cuts = apex::separate_blossom(g, cfg);
        if (best > 1e-5) {
            ++with_violation;
            REQUIRE_FALSE(cuts.empty());
            CHECK(cuts.front().violation == doctest::Approx(best));
        } else {
            CHECK(cuts.empty());
        }
    }
    MESSAGE("points with a violated blossom: " << with_violation << "/60");
    CHECK(with_violation > 5);
}

TEST_CASE("T_blossom_never_invalid") {
    // 1000 random support graphs: every emitted cut is violated by x (exact recompute), has an odd
    // tooth set inside delta(H), and is satisfied by random Hamiltonian cycles (validity).
    std::mt19937_64 rng(1000);
    uint64_t emitted = 0, invalid = 0;
    for (int trial = 0; trial < 1000; ++trial) {
        const uint32_t n = 10 + static_cast<uint32_t>(rng() % 15);
        const auto p = trial % 3 == 0 ? random_point(rng, n) : half_integral(rng, n, trial % 3 == 2);
        const auto g = apex::build_support(n, p.edges, p.x);
        BlossomConfig cfg;
        cfg.mode = trial % 4 == 0 ? BlossomMode::Full : BlossomMode::Heuristic;
        for (const auto& c : apex::separate_blossom(g, cfg)) {
            ++emitted;
            invalid += !(apex::blossom_violation(g, c.handle, c.teeth) > 1e-5);
            invalid += c.teeth.size() % 2 == 0;
            std::vector<uint8_t> in(n, 0);
            for (uint32_t v : c.handle) in[v] = 1;
            for (const auto& t : c.teeth) invalid += in[t.u] == in[t.v];
            for (int k = 0; k < 30; ++k) {
                std::vector<uint32_t> tour(n);
                for (uint32_t i = 0; i < n; ++i) tour[i] = i;
                for (uint32_t i = n - 1; i > 0; --i) std::swap(tour[i], tour[rng() % (i + 1)]);
                invalid += !satisfied_by_tour(c, tour, n);
            }
        }
    }
    MESSAGE("emitted=" << emitted << " invalid=" << invalid);
    CHECK(emitted > 100);
    CHECK(invalid == 0);
}
