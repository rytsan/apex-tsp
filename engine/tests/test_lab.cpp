// Heuristics lab steps (Sprint 6 M6.4): every step returns a valid tour with its true cost.
#include <doctest/doctest.h>

#include <string>

#include "graph/candidate_graph.hpp"
#include "primal/lab.hpp"

namespace {
apex::Instance load(const std::string& name) { return apex::read_tsplib(std::string(APEX_DATA_DIR) + "/" + name + ".tsp"); }
}  // namespace

TEST_CASE("T_lab_steps_valid_tours") {
    const auto inst = load("a280");
    const auto g = apex::build_candidate_graph(inst);
    apex::Tour t;
    auto none = apex::run_lab_step("two_opt", t, inst, g, 1);
    CHECK(!none.ok);  // improvement before any construction
    CHECK(none.error.find("no tour") != std::string::npos);
    for (const char* construct : {"nn", "nn 17", "greedy", "hilbert", "random", "decomp"}) {
        CAPTURE(construct);
        auto s = apex::run_lab_step(construct, t, inst, g, 7);
        REQUIRE(s.ok);
        CHECK(apex::is_valid_tour(t, inst.n));
        CHECK(t.cost == apex::tour_cost(inst, t));
        CHECK(s.after == t.cost);
        int64_t prev = t.cost;
        for (const char* improve : {"two_opt", "two_opt_oropt", "lk", "ils 0.2"}) {
            CAPTURE(improve);
            auto st = apex::run_lab_step(improve, t, inst, g, 7);
            REQUIRE(st.ok);
            CHECK(apex::is_valid_tour(t, inst.n));
            CHECK(t.cost == apex::tour_cost(inst, t));
            CHECK(st.before == prev);
            CHECK(t.cost <= prev);  // local search never worsens the tour
            prev = t.cost;
        }
        CHECK(t.cost >= 2579);
    }
    CHECK(apex::run_lab_step("nn", t, inst, g, 1).after > apex::run_lab_step("lk", t, inst, g, 1).after);
    CHECK(!apex::run_lab_step("bogus", t, inst, g, 1).ok);
    CHECK(!apex::run_lab_step("ils -1", t, inst, g, 1).ok);
    // nn from a fixed start is deterministic and starts there
    const auto a = apex::nearest_neighbor_tour(inst, g, 5), b = apex::nearest_neighbor_tour(inst, g, 5);
    CHECK(a.order == b.order);
    CHECK(a.order[0] == 5);
}
