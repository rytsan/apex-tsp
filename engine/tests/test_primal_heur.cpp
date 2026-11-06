#include <doctest/doctest.h>

#include <string>

#include "bnc/primal_heur.hpp"
#include "bnc/tree.hpp"
#include "geo/hilbert.hpp"
#include "primal/two_opt.hpp"

namespace {

apex::Instance load(const std::string& name) {
    return apex::read_tsplib(std::string(APEX_DATA_DIR) + "/" + name + ".tsp");
}

bool fractional(const std::vector<double>& x) {
    for (double v : x)
        if (v > 1e-6 && v < 1 - 1e-6) return true;
    return false;
}

}  // namespace

TEST_CASE("T_diving_finds_tour_small") {
    // eil51 B&C with the other primal sources off (no LP-guided tour, no RIH, weak start, no
    // blossoms): diving must find a tour within 1000 processed nodes and the proof must hold.
    const auto inst = load("eil51");
    auto g = apex::build_candidate_graph(inst);
    apex::BnCConfig cfg;
    cfg.root.use_blossom = false;
    cfg.lp_heuristic = false;
    cfg.rih_iterations = 0;
    cfg.max_nodes = 1000;
    cfg.dive.max_rounds = 5;  // off by default (README §9)
    const auto res = apex::run_bnc(inst, g, cfg, apex::hilbert_tour(inst, apex::hilbert_order(inst)));
    CHECK(res.dive_calls > 0);
    CHECK(res.dive_tours > 0);
    CHECK(res.nodes <= 1000);
    CHECK(res.status == apex::BnCResult::Status::Optimal);
    CHECK(res.ub == 426);
    MESSAGE("eil51 nodes=" << res.nodes << " dive_calls=" << res.dive_calls << " dive_tours=" << res.dive_tours
                           << " dive_pivots=" << res.dive_pivots);

    // Dive alone never changes the LP bounds it borrows.
    auto g2 = apex::build_candidate_graph(inst);
    apex::RootLoopConfig rc;
    rc.use_blossom = false;
    apex::CutLoop loop(inst, g2, rc);
    const auto r = loop.solve({}, nullptr, apex::NodeLimits{});
    const auto lower = loop.lp().lower;
    const auto upper = loop.lp().upper;
    if (fractional(r.final.x)) apex::dive(inst, g2, loop.lp_mut(), loop.basis().basis, r.final.x, rc.lp, apex::DiveConfig{});
    CHECK(loop.lp().lower == lower);
    CHECK(loop.lp().upper == upper);
}

TEST_CASE("T_rih_never_worsens_ub") {
    for (const char* name : {"eil51", "berlin52", "a280", "pr1002"}) {
        CAPTURE(name);
        const auto inst = load(name);
        auto g = apex::build_candidate_graph(inst);
        apex::RootLoopConfig cfg;
        apex::CutLoop loop(inst, g, cfg);
        apex::NodeLimits lim;
        lim.max_rounds = 5;
        const auto r = loop.solve({}, nullptr, lim);
        apex::IlsConfig ic;
        ic.max_iterations = 200;
        auto weak = apex::hilbert_tour(inst, apex::hilbert_order(inst));
        apex::two_opt_oropt(weak, inst, g, apex::TwoOptConfig{});
        const auto a = apex::rih_lite(inst, g, r.final.x, weak, ic);  // usually improves
        CHECK(a.cost <= weak.cost);
        CHECK(a.cost == apex::tour_cost(inst, a));
        REQUIRE(apex::is_valid_tour(a, inst.n));
        auto strong = a;  // an incumbent RIH cannot beat must come back unchanged
        strong.cost = 0;
        const auto b = apex::rih_lite(inst, g, r.final.x, strong, ic);
        CHECK(b.order == strong.order);
        CHECK(b.cost == 0);
        MESSAGE(std::string(name) << " two_opt=" << weak.cost << " rih=" << a.cost);
    }
}
