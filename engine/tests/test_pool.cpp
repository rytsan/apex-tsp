#include <doctest/doctest.h>

#include <algorithm>
#include <random>
#include <thread>
#include <vector>

#include "cuts/pool.hpp"

using apex::CutKind;
using apex::CutPool;
using apex::Edge;

namespace {
std::vector<Edge> ring_edges(uint32_t n) {  // cycle 0..n-1 plus chords (i, i+2)
    std::vector<Edge> e;
    for (uint32_t i = 0; i < n; ++i) {
        e.push_back(Edge{std::min(i, (i + 1) % n), std::max(i, (i + 1) % n), 1, apex::kActive, 0.0});
        e.push_back(Edge{std::min(i, (i + 2) % n), std::max(i, (i + 2) % n), 1, apex::kActive, 0.0});
    }
    return e;
}
}  // namespace

TEST_CASE("T_pool_dedup_aging") {
    CutPool pool(3);
    pool.stage_sec({{3, 1, 2}, 0.5});
    pool.stage_sec({{1, 2, 3}, 0.7});  // same set, different order: duplicate
    pool.stage_sec({{4, 5}, 0.2});
    apex::BlossomCut b{{0, 1, 2}, {{0, 9, 7}, {1, 8, 5}, {2, 7, 3}}, 1.0};
    pool.stage_blossom(b);
    pool.stage_blossom(b);  // duplicate in the same batch
    auto added = pool.commit();
    CHECK(added.size() == 3);
    pool.stage_sec({{2, 3, 1}, 0.1});  // duplicate of an existing pool cut
    CHECK(pool.commit().empty());
    auto st = pool.stats();
    CHECK(st.size == 3);
    CHECK(st.duplicates == 3);

    // Rows: SEC x(delta(S)) >= 2; blossom teeth get -1 and rhs 1 - |T|.
    const auto edges = ring_edges(10);
    for (size_t i = 0; i < pool.size(); ++i) {
        const auto r = pool.make_row(pool.at(i), edges);
        CHECK(r.sense == apex::Sense::Ge);
        if (pool.at(i).kind == CutKind::Sec) CHECK(r.rhs == 2.0);
        else CHECK(r.rhs == -2.0);
    }

    // Aging: cut 0 always zero dual -> leaves after 3 re-solves; cut 1 active resets.
    pool.set_lp_row(0, 0);
    pool.set_lp_row(1, 1);
    std::vector<double> duals = {0.0, 0.0};
    CHECK(pool.age(duals).empty());
    duals[1] = 2.5;
    CHECK(pool.age(duals).empty());
    duals[1] = 0.0;
    const auto out = pool.age(duals);  // cut 0: 3rd zero round; cut 1: 1st
    REQUIRE(out.size() == 1);
    CHECK(out[0] == 0);
    pool.set_lp_row(0, -1);
    CHECK(pool.stats().in_lp == 1);
    CHECK(pool.stats().aged_out == 1);

    // Back-to-LP: a pool cut outside the LP that becomes violated is reported.
    std::vector<double> x(edges.size(), 0.0);  // x = 0: every SEC violated by 2
    const auto viol = pool.violated_outside(edges, x);
    CHECK(std::find(viol.begin(), viol.end(), size_t{0}) != viol.end());
    CHECK(std::find(viol.begin(), viol.end(), size_t{1}) == viol.end());  // still in the LP
    pool.set_lp_row(0, 0);
    CHECK(pool.at(0).inactive_rounds == 0);  // re-entry resets the age
}

TEST_CASE("T_pool_deterministic_merge") {
    // 4 threads stage the same cut set in different interleavings: committed order and ids are
    // identical to a single-threaded run.
    std::vector<apex::SecCut> cuts;
    std::mt19937_64 rng(6);
    for (int k = 0; k < 400; ++k) {
        std::vector<uint32_t> s;
        const uint32_t sz = 2 + rng() % 8;
        for (uint32_t i = 0; i < sz; ++i) s.push_back(static_cast<uint32_t>(rng() % 50));
        std::sort(s.begin(), s.end());
        s.erase(std::unique(s.begin(), s.end()), s.end());
        cuts.push_back({s, 0.1});
    }
    auto run = [&](int threads, uint64_t seed) {
        CutPool pool;
        std::vector<std::thread> ts;
        for (int t = 0; t < threads; ++t)
            ts.emplace_back([&, t] {
                std::vector<size_t> order(cuts.size());
                for (size_t i = 0; i < order.size(); ++i) order[i] = i;
                std::mt19937_64 r(seed + t);
                std::shuffle(order.begin(), order.end(), r);
                for (size_t i = t; i < order.size(); i += threads) pool.stage_sec(cuts[order[i]]);
            });
        for (auto& th : ts) th.join();
        // Each cut is staged by exactly one thread only if threads partition indices; stage all
        // remaining from the main thread too, to exercise dedup with concurrency.
        for (const auto& c : cuts) pool.stage_sec(c);
        pool.commit();
        std::vector<std::vector<uint32_t>> seq;
        for (size_t i = 0; i < pool.size(); ++i) {
            CHECK(pool.at(i).id == i);
            seq.push_back(pool.at(i).nodes);
        }
        return seq;
    };
    const auto ref = run(1, 1);
    CHECK(run(4, 7) == ref);
    CHECK(run(4, 99) == ref);
    CHECK(run(3, 5) == ref);
    CHECK(!ref.empty());
}
