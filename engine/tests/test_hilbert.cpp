#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <random>
#include <string>

#include "core/instance.hpp"
#include "geo/hilbert.hpp"

using apex::hilbert_d2xy;
using apex::hilbert_xy2d;

TEST_CASE("T_hilbert_roundtrip") {
    std::mt19937_64 rng(12345);
    uint64_t bad_key = 0, bad_xy = 0, bad_adjacent = 0;
    for (int it = 0; it < 1000000; ++it) {
        const uint32_t key = static_cast<uint32_t>(rng());
        uint32_t u = 0, v = 0;
        hilbert_d2xy(key, u, v);
        if (u >= apex::kHilbertSide || v >= apex::kHilbertSide || hilbert_xy2d(u, v) != key) ++bad_key;

        const uint32_t pu = static_cast<uint32_t>(rng()) & 0xFFFFu, pv = static_cast<uint32_t>(rng()) & 0xFFFFu;
        uint32_t qu = 0, qv = 0;
        hilbert_d2xy(hilbert_xy2d(pu, pv), qu, qv);
        if (qu != pu || qv != pv) ++bad_xy;

        // Curve continuity: consecutive keys are 4-neighbours on the grid.
        if (key != 0xFFFFFFFFu) {
            uint32_t nu = 0, nv = 0;
            hilbert_d2xy(key + 1, nu, nv);
            const auto du = std::abs(static_cast<int64_t>(nu) - u), dv = std::abs(static_cast<int64_t>(nv) - v);
            if (du + dv != 1) ++bad_adjacent;
        }
    }
    CHECK(bad_key == 0);
    CHECK(bad_xy == 0);
    CHECK(bad_adjacent == 0);
    CHECK(hilbert_xy2d(0, 0) == 0);
}

TEST_CASE("T_hilbert_key_clamps_to_grid") {
    CHECK(apex::hilbert_key(0, 0, 0, 0, 10, 10) == hilbert_xy2d(0, 0));
    uint32_t u = 0, v = 0;
    hilbert_d2xy(apex::hilbert_key(10, 10, 0, 0, 10, 10), u, v);
    CHECK(u == apex::kHilbertSide - 1);
    CHECK(v == apex::kHilbertSide - 1);
    CHECK(apex::hilbert_key(5, 5, 5, 5, 5, 5) == 0);  // degenerate box
}

namespace {
// ratio = mean grid distance of Hilbert-adjacent entries / mean distance of random pairs
double locality_ratio(const apex::Instance& inst) {
    const auto order = apex::hilbert_order(inst);
    REQUIRE(order.size() == inst.n);
    std::vector<uint8_t> seen(inst.n, 0);
    for (auto i : order) { REQUIRE(i < inst.n); REQUIRE(seen[i] == 0); seen[i] = 1; }

    auto dist = [&](uint32_t a, uint32_t b) {
        return std::hypot(inst.xs[a] - inst.xs[b], inst.ys[a] - inst.ys[b]);
    };
    double adj = 0;
    for (uint32_t k = 0; k + 1 < inst.n; ++k) adj += dist(order[k], order[k + 1]);
    adj /= inst.n - 1;

    std::mt19937_64 rng(7);
    std::uniform_int_distribution<uint32_t> pick(0, inst.n - 1);
    double glob = 0;
    const int samples = 200000;
    for (int s = 0; s < samples; ++s) glob += dist(pick(rng), pick(rng));
    glob /= samples;
    MESSAGE(inst.name << ": adjacent=" << adj << " global=" << glob << " ratio=" << adj / glob);
    return adj / glob;
}
}  // namespace

TEST_CASE("T_hilbert_locality") {
    apex::Instance uni;
    uni.name = "uniform100k";
    uni.n = 100000;
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> U(0.0, 1e6);
    for (uint32_t i = 0; i < uni.n; ++i) {
        uni.xs.push_back(U(rng));
        uni.ys.push_back(U(rng));
        uni.original_ids.push_back(i + 1);
    }
    CHECK(locality_ratio(uni) < 0.02);

    const auto ml = apex::read_tsplib(std::string(APEX_DATA_DIR) + "/mona-lisa100K.tsp");
    CHECK(locality_ratio(ml) < 0.02);
    const auto pr = apex::read_tsplib(std::string(APEX_DATA_DIR) + "/pr1002.tsp");
    CHECK(locality_ratio(pr) < 0.15);
}
