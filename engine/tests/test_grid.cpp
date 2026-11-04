#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <string>
#include <utility>

#include "core/instance.hpp"
#include "geo/grid.hpp"

namespace {

apex::Instance make(std::vector<double> xs, std::vector<double> ys) {
    apex::Instance inst;
    inst.name = "synthetic";
    inst.n = static_cast<uint32_t>(xs.size());
    inst.xs = std::move(xs);
    inst.ys = std::move(ys);
    for (uint32_t i = 0; i < inst.n; ++i) inst.original_ids.push_back(i + 1);
    return inst;
}

double d2(const apex::Instance& in, uint32_t a, uint32_t b) {
    const double dx = in.xs[a] - in.xs[b], dy = in.ys[a] - in.ys[b];
    return dx * dx + dy * dy;
}

std::vector<uint32_t> brute_knn(const apex::Instance& in, uint32_t i, uint32_t k) {
    std::vector<std::pair<double, uint32_t>> all;
    for (uint32_t j = 0; j < in.n; ++j)
        if (j != i) all.emplace_back(d2(in, i, j), j);
    std::sort(all.begin(), all.end());
    std::vector<uint32_t> out;
    for (uint32_t t = 0; t < std::min<size_t>(k, all.size()); ++t) out.push_back(all[t].second);
    return out;
}

// Mix of distributions: uniform, clustered, integer lattice (many ties), collinear, duplicates.
std::vector<apex::Instance> fixtures() {
    std::mt19937_64 rng(2024);
    std::vector<apex::Instance> v;
    for (uint32_t n : {2u, 3u, 17u, 300u, 2000u}) {
        std::uniform_real_distribution<double> U(-500, 1500);
        std::vector<double> x, y;
        for (uint32_t i = 0; i < n; ++i) { x.push_back(U(rng)); y.push_back(U(rng)); }
        v.push_back(make(x, y));
    }
    {
        std::normal_distribution<double> N(0, 1);
        std::vector<double> x, y;
        for (int c = 0; c < 10; ++c)
            for (int i = 0; i < 150; ++i) { x.push_back(c * 1e4 + N(rng)); y.push_back((c % 3) * 1e4 + N(rng)); }
        v.push_back(make(x, y));
    }
    {
        std::uniform_int_distribution<int> L(0, 20);
        std::vector<double> x, y;
        for (int i = 0; i < 800; ++i) { x.push_back(L(rng)); y.push_back(L(rng)); }  // ties + duplicates
        v.push_back(make(x, y));
    }
    {
        std::vector<double> x, y;
        for (int i = 0; i < 500; ++i) { x.push_back(i * 3.0); y.push_back(7.0); }  // collinear
        v.push_back(make(x, y));
    }
    v.push_back(make(std::vector<double>(50, 1.0), std::vector<double>(50, 1.0)));  // all equal
    return v;
}

}  // namespace

TEST_CASE("T_grid_knn_bruteforce") {
    uint64_t mismatches = 0, queries = 0;
    for (const auto& inst : fixtures()) {
        const apex::SpatialGrid g(inst);
        std::vector<uint32_t> got;
        for (uint32_t k : {1u, 8u, 10u, 40u}) {
            for (uint32_t i = 0; i < inst.n; ++i) {
                g.knn(i, k, got);
                ++queries;
                if (got != brute_knn(inst, i, k)) ++mismatches;
            }
        }
    }
    MESSAGE("knn queries=" << queries);
    CHECK(mismatches == 0);
}

TEST_CASE("T_grid_radius_bruteforce") {
    uint64_t mismatches = 0;
    std::mt19937_64 rng(99);
    for (const auto& inst : fixtures()) {
        const apex::SpatialGrid g(inst);
        for (uint32_t i = 0; i < inst.n; i += 7) {
            for (double r : {0.0, 1.0, 5.0, 100.0, 1e5}) {
                std::vector<uint32_t> got, want;
                g.for_each_in_radius(i, r, [&](uint32_t j) { got.push_back(j); });
                for (uint32_t j = 0; j < inst.n; ++j)
                    if (j != i && d2(inst, i, j) <= r * r) want.push_back(j);
                std::sort(got.begin(), got.end());
                if (got != want) ++mismatches;
            }
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("T_grid_knn_scale") {
    const auto inst = apex::read_tsplib(std::string(APEX_DATA_DIR) + "/mona-lisa100K.tsp");
    const auto t0 = std::chrono::steady_clock::now();
    const apex::SpatialGrid g(inst);
    const auto t1 = std::chrono::steady_clock::now();
    std::vector<uint32_t> out;
    uint64_t total = 0;
    for (uint32_t i = 0; i < inst.n; ++i) {
        g.knn(i, 8, out);
        total += out.size();
    }
    const auto t2 = std::chrono::steady_clock::now();
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    MESSAGE("mona-lisa100K grid " << g.cells_x() << "x" << g.cells_y() << " build_ms=" << ms(t0, t1)
                                  << " knn8_all_ms=" << ms(t1, t2));
    CHECK(total == 8ull * inst.n);
    // Spot-check against brute force.
    for (uint32_t i = 0; i < inst.n; i += 9973) {
        g.knn(i, 8, out);
        CHECK(out == brute_knn(inst, i, 8));
    }
}
