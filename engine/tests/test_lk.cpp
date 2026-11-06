#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "core/tour_io.hpp"
#include "geo/hilbert.hpp"
#include "primal/lk_lite.hpp"
#include "primal/two_opt.hpp"

namespace {

apex::Instance load(const std::string& name) {
    return apex::read_tsplib(std::string(APEX_DATA_DIR) + "/" + name + ".tsp");
}

apex::Tour start_tour(const apex::Instance& inst) { return apex::hilbert_tour(inst, apex::hilbert_order(inst)); }

}  // namespace

TEST_CASE("T_ils_improves_monotone") {
    for (const char* name : {"berlin52", "a280"}) {
        CAPTURE(name);
        const auto inst = load(name);
        const auto g = apex::build_candidate_graph(inst);
        auto t = start_tour(inst);
        const int64_t start = t.cost;
        apex::IlsConfig cfg;
        cfg.max_iterations = 100;
        cfg.publish_every = 1;
        cfg.strong_kick_after = 20;  // exercises the strong kick within 100 iterations
        std::vector<int64_t> seen;
        const auto st = apex::iterated_local_search(t, inst, g, cfg, nullptr, [&](const apex::Tour& b) {
            CHECK(b.cost == apex::tour_cost(inst, b));
            seen.push_back(b.cost);
        });
        REQUIRE(apex::is_valid_tour(t, inst.n));
        CHECK(t.cost == apex::tour_cost(inst, t));
        CHECK(st.iterations == 100);
        CHECK(st.best_cost == t.cost);
        CHECK(t.cost <= start);
        REQUIRE(!seen.empty());
        for (size_t k = 1; k < seen.size(); ++k) CHECK(seen[k] <= seen[k - 1]);
        MESSAGE(std::string(name) << " start=" << start << " best=" << t.cost << " improvements=" << st.improvements
                     << " strong_kicks=" << st.strong_kicks << " lk_moves=" << st.lk_moves);
    }
}

TEST_CASE("T_lk_deterministic") {
    const auto inst = load("pr1002");
    const auto g = apex::build_candidate_graph(inst);
    apex::IlsConfig cfg;
    cfg.max_iterations = 2000;
    auto a = start_tour(inst), b = a;
    apex::iterated_local_search(a, inst, g, cfg);
    apex::iterated_local_search(b, inst, g, cfg);
    CHECK(a.order == b.order);
    CHECK(a.cost == b.cost);
    cfg.seed = 2;
    auto c = start_tour(inst);
    apex::iterated_local_search(c, inst, g, cfg);
    CHECK(c.cost == apex::tour_cost(inst, c));
    // LK-lite + ILS must beat plain 2-opt + Or-opt from the same start.
    auto d = start_tour(inst);
    apex::two_opt_oropt(d, inst, g, apex::TwoOptConfig{});
    CHECK(a.cost < d.cost);
    MESSAGE("pr1002 ils seed1=" << a.cost << " seed2=" << c.cost << " two_opt=" << d.cost << " opt=259045");
}

TEST_CASE("T_background_thread_no_races") {
    // Run under TSan: the consumer polls while the ILS thread publishes.
    const auto inst = load("a280");
    const auto g = apex::build_candidate_graph(inst);
    const auto start = start_tour(inst);
    apex::IlsConfig cfg;
    cfg.publish_every = 1;
    apex::BackgroundIls bg(inst, g, start, cfg);
    int64_t ub = start.cost;
    uint32_t taken = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500)) {
        apex::Tour t;
        if (bg.take_if_better(ub, t)) {
            REQUIRE(apex::is_valid_tour(t, inst.n));
            CHECK(apex::tour_cost(inst, t) == t.cost);
            CHECK(t.cost < ub);
            ub = t.cost;
            ++taken;
        }
        std::this_thread::yield();
    }
    bg.stop();
    const auto st = bg.stats();
    CHECK(taken > 0);
    CHECK(st.best_cost == bg.best_cost());
    CHECK(bg.best_cost() <= ub);
    CHECK(st.iterations > 0);
    MESSAGE("a280 taken=" << taken << " best=" << bg.best_cost() << " iterations=" << st.iterations << " opt=2579");
}
