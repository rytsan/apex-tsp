#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <set>
#include <string>

#include "bnc/pricing.hpp"
#include "bnc/tree.hpp"
#include "core/tour_io.hpp"
#include "geo/grid.hpp"
#include "geo/hilbert.hpp"
#include "primal/two_opt.hpp"

namespace {

apex::Instance load(const std::string& name) {
    return apex::read_tsplib(std::string(APEX_DATA_DIR) + "/" + name + ".tsp");
}

// Duals shaped like a subtour LP (about half the distance to the 2nd nearest neighbour, with
// noise) plus random Hilbert-chunk cuts, a few with tiny negative duals.
struct Synthetic {
    std::vector<double> y;
    std::vector<std::vector<uint32_t>> sets;
    std::vector<apex::CutDual> cuts;
};

Synthetic synthetic_duals(const apex::Instance& inst, uint32_t seed) {
    Synthetic s;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> noise(0.9, 1.4), cy(0.0, 20.0);
    const apex::SpatialGrid grid(inst);
    std::vector<uint32_t> nb;
    s.y.resize(inst.n);
    for (uint32_t v = 0; v < inst.n; ++v) {
        grid.knn(v, 2, nb);
        s.y[v] = 0.5 * apex::dist(inst, v, nb.back()) * noise(rng);
    }
    const auto order = apex::hilbert_order(inst);
    std::uniform_int_distribution<uint32_t> start(0, inst.n - 40), len(3, 40);
    for (int k = 0; k < 300; ++k) {
        const uint32_t a = start(rng), l = len(rng);
        std::vector<uint32_t> set(order.begin() + a, order.begin() + a + l);
        std::sort(set.begin(), set.end());
        s.sets.push_back(std::move(set));
    }
    for (size_t k = 0; k < s.sets.size(); ++k) s.cuts.push_back({&s.sets[k], k % 50 == 0 ? -1e-8 : cy(rng)});
    return s;
}

bool same_edges(std::vector<apex::NegEdge> a, std::vector<apex::NegEdge> b) {
    auto key = [](const apex::NegEdge& e) { return std::make_pair(e.u, e.v); };
    auto less = [&](const apex::NegEdge& x, const apex::NegEdge& y) { return key(x) < key(y); };
    std::sort(a.begin(), a.end(), less);
    std::sort(b.begin(), b.end(), less);
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); ++k)
        if (key(a[k]) != key(b[k]) || a[k].rc != b[k].rc) return false;
    return true;
}

apex::Tour heuristic_tour(const apex::Instance& inst, const apex::CandidateGraph& g) {
    auto t = apex::hilbert_tour(inst, apex::hilbert_order(inst));
    apex::two_opt_oropt(t, inst, g, apex::TwoOptConfig{});
    return t;
}

}  // namespace

TEST_CASE("T_pricing_grid_equals_scan") {
    // The grid radius is an exact bound: grid, grid-scalar and the full O(n^2) scan must report
    // the same negative edges with bitwise identical reduced costs.
    for (const char* name : {"pr1002", "fl3795", "d15112"}) {
        CAPTURE(name);
        const auto inst = load(name);
        const auto g = apex::build_candidate_graph(inst);
        std::unordered_set<uint64_t> cand;
        for (const auto& e : g.edges) cand.insert((uint64_t(e.u) << 32) | e.v);
        const auto s = synthetic_duals(inst, 7);
        apex::EdgePricer pricer(inst);
        std::vector<apex::NegEdge> grid, scalar, scan;
        uint64_t cg = 0, cs = 0, cn = 0;
        const auto t0 = std::chrono::steady_clock::now();
        const double sg = pricer.price(s.y, s.cuts, cand, false, apex::PricingMode::Grid, &grid, cg);
        const auto t1 = std::chrono::steady_clock::now();
        const double ss = pricer.price(s.y, s.cuts, cand, false, apex::PricingMode::GridScalar, &scalar, cs);
        const double sn = pricer.price(s.y, s.cuts, cand, false, apex::PricingMode::Scan, &scan, cn);
        const auto t2 = std::chrono::steady_clock::now();
        CHECK(cg > 0);
        CHECK(cg == cn);
        CHECK(cs == cn);
        CHECK(same_edges(grid, scan));
        CHECK(same_edges(scalar, scan));
        CHECK(sg == doctest::Approx(sn).epsilon(1e-12));
        CHECK(ss == doctest::Approx(sn).epsilon(1e-12));
        MESSAGE(std::string(name) << " negative=" << cn << " grid_s=" << std::chrono::duration<double>(t1 - t0).count()
                     << " scalar+scan_s=" << std::chrono::duration<double>(t2 - t1).count());
    }
}

TEST_CASE("T_pricing_simd_parity") {
    if (!apex::pricing_simd_available()) {
        MESSAGE("AVX2 not compiled in: SIMD path is the scalar fallback (parity trivially holds)");
    }
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> c(0, 1000), yv(-50, 200);
    uint64_t kept = 0, total = 0;
    for (int trial = 0; trial < 2000; ++trial) {
        const uint32_t m = 1 + trial % 37;  // exercises the scalar tail
        std::vector<double> xs(m), ys(m), Y(m);
        for (uint32_t k = 0; k < m; ++k) { xs[k] = c(rng); ys[k] = c(rng); Y[k] = yv(rng); }
        const double xu = c(rng), yu = c(rng), Yu = yv(rng);
        std::vector<uint8_t> a(m), b(m);
        apex::pricing_filter_scalar(xu, yu, Yu, xs.data(), ys.data(), Y.data(), m, a.data());
        apex::pricing_filter_simd(xu, yu, Yu, xs.data(), ys.data(), Y.data(), m, b.data());
        REQUIRE(a == b);
        for (auto v : a) kept += v;
        total += m;
    }
    MESSAGE("simd=" << apex::pricing_simd_available() << " pairs=" << total << " kept=" << kept);
    CHECK(kept > 0);
    CHECK(kept < total);
}

TEST_CASE("T_verification_pricing_planted_edge") {
    // Remove edges of the published optimal tour from eil51's candidate graph: the solver must
    // price them back (exact complete-graph pricing) and still prove 426.
    const auto inst = load("eil51");
    const auto opt = apex::read_tour(std::string(APEX_DATA_DIR) + "/eil51.opt.tour", inst);
    std::set<std::pair<uint32_t, uint32_t>> removed;
    for (size_t k = 0; k < opt.order.size(); k += 10) {
        const uint32_t a = opt.order[k], b = opt.order[(k + 1) % opt.order.size()];
        removed.insert({std::min(a, b), std::max(a, b)});
    }
    auto g = apex::build_candidate_graph(inst);
    std::vector<apex::Edge> kept;
    for (const auto& e : g.edges)
        if (!removed.count({e.u, e.v})) kept.push_back(e);
    REQUIRE(kept.size() + removed.size() == g.edges.size());
    g.edges = kept;
    apex::rebuild_adjacency(g);
    const auto res = apex::run_bnc(inst, g, apex::BnCConfig{}, heuristic_tour(inst, g));
    CHECK(res.status == apex::BnCResult::Status::Optimal);
    CHECK(res.ub == 426);
    CHECK(res.lb_int == 426);
    uint32_t back = 0;
    for (const auto& e : g.edges)
        if (removed.count({e.u, e.v})) {
            CHECK((e.status & apex::kNewPricing) != 0);
            ++back;
        }
    CHECK(back > 0);
    MESSAGE("removed=" << removed.size() << " priced back=" << back << " verification_calls=" << res.verification_pricing_calls);
}
