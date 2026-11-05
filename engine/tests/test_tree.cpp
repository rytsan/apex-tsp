#include <doctest/doctest.h>

#include <string>

#include "bnc/tree.hpp"
#include "core/tour_io.hpp"
#include "geo/hilbert.hpp"
#include "primal/two_opt.hpp"

namespace {
apex::Instance load(const std::string& name) {
    return apex::read_tsplib(std::string(APEX_DATA_DIR) + "/" + name + ".tsp");
}
apex::Tour heuristic_tour(const apex::Instance& inst, const apex::CandidateGraph& g) {
    auto t = apex::hilbert_tour(inst, apex::hilbert_order(inst));
    apex::two_opt_oropt(t, inst, g, apex::TwoOptConfig{});
    return t;
}
}  // namespace

TEST_CASE("T_farkas_pricing_repairs_restricted_infeasibility") {
    // Fixing all but one candidate edge at node 0 to zero makes the candidate LP infeasible
    // (degree 2 impossible), but the complete graph is still feasible: Farkas pricing must add
    // repairing columns instead of declaring the node infeasible.
    const auto inst = load("eil51");
    auto g = apex::build_candidate_graph(inst);
    apex::RootLoopConfig cfg;
    apex::CutLoop loop(inst, g, cfg);
    apex::BoundChanges bc;
    bool kept = false;
    for (uint32_t k = g.adj_start[0]; k < g.adj_start[1]; ++k) {
        if (!kept) { kept = true; continue; }
        bc.push_back({g.adj[k], 0});
    }
    const size_t before = g.edges.size();
    const auto r = loop.solve(bc, nullptr, apex::NodeLimits{});
    CHECK_FALSE(r.infeasible);
    CHECK(g.edges.size() > before);
    CHECK(r.converged);
    CHECK(std::isfinite(r.global_lb));

    // Three edges at node 0 forced to 1: infeasible over the complete graph too.
    apex::BoundChanges bad;
    for (uint32_t k = g.adj_start[0]; k < g.adj_start[0] + 3; ++k) bad.push_back({g.adj[k], 1});
    const auto r2 = loop.solve(bad, nullptr, apex::NodeLimits{});
    CHECK(r2.infeasible);
}

TEST_CASE("T_bnc_proves_optimality_small") {
    struct Ref { const char* name; int64_t opt; };
    for (const auto& r : {Ref{"eil51", 426}, Ref{"berlin52", 7542}, Ref{"a280", 2579}}) {
        CAPTURE(r.name);
        const auto inst = load(r.name);
        auto g = apex::build_candidate_graph(inst);
        apex::BnCConfig cfg;
        const auto res = apex::run_bnc(inst, g, cfg, heuristic_tour(inst, g));
        CHECK(res.status == apex::BnCResult::Status::Optimal);
        CHECK(res.ub == r.opt);
        CHECK(res.lb_int == res.ub);
        CHECK(apex::is_valid_tour(res.best, inst.n));
        CHECK(apex::tour_cost(inst, res.best) == res.ub);  // certificate: recomputed with the metric
        CHECK(res.warm_dual_infeasible == 0);
        CHECK(res.root_lb <= r.opt + 1e-6);
        MESSAGE(std::string(r.name) << " nodes=" << res.nodes << " branched=" << res.branched << " depth=" << res.max_depth
                       << " root_lb=" << res.root_lb << " warm=" << res.warm_starts << " t=" << res.total_s);
    }
}

TEST_CASE("T_cutloop_resolve_after_cutoff_with_new_columns") {
    // A call stopped by the cutoff right after pricing added columns must leave the loop usable.
    const auto inst = load("pr1002");
    auto g = apex::build_candidate_graph(inst);
    apex::RootLoopConfig cfg;
    apex::CutLoop loop(inst, g, cfg);
    apex::NodeLimits lim;
    lim.cutoff = 1;  // every Farley bound reaches it: stop at the first priced round
    const size_t before = g.edges.size();
    const auto r = loop.solve({}, nullptr, lim);
    CHECK(r.cutoff);
    CHECK(g.edges.size() > before);  // pr1002 root prices columns in round 0
    const auto r2 = loop.solve({}, nullptr, apex::NodeLimits{});
    CHECK(r2.converged);
    CHECK(loop.lp().c.size() == g.edges.size());
}

TEST_CASE("T_bnc_time_limit_anytime") {
    const auto inst = load("pr1002");
    auto g = apex::build_candidate_graph(inst);
    apex::BnCConfig cfg;
    cfg.time_limit_s = 2.0;
    const auto res = apex::run_bnc(inst, g, cfg, heuristic_tour(inst, g));
    CHECK(res.lb <= 259045 + 1e-6);
    CHECK(res.lb <= res.ub + 1e-6);
    CHECK(apex::tour_cost(inst, res.best) == res.ub);
    if (res.status != apex::BnCResult::Status::Optimal) CHECK(res.status == apex::BnCResult::Status::TimeLimit);
}

TEST_CASE("T_branching_deterministic") {
    // Same seed, one thread => identical node processing order and result (reliability branching).
    for (const char* name : {"eil51", "a280"}) {
        CAPTURE(name);
        const auto inst = load(name);
        std::vector<uint32_t> first;
        int64_t ub = 0;
        for (int run = 0; run < 2; ++run) {
            auto g = apex::build_candidate_graph(inst);
            const auto res = apex::run_bnc(inst, g, apex::BnCConfig{}, heuristic_tour(inst, g));
            if (run == 0) { first = res.processing_order; ub = res.ub; continue; }
            CHECK(res.processing_order == first);
            CHECK(res.ub == ub);
        }
    }
}

TEST_CASE("T_strong_branching_budget") {
    const auto inst = load("a280");
    {  // inside the tree: no node exceeds the per-node budget
        auto g = apex::build_candidate_graph(inst);
        apex::BnCConfig cfg;
        const auto res = apex::run_bnc(inst, g, cfg, heuristic_tour(inst, g));
        CHECK(res.strong_candidates > 0);
        CHECK(res.max_node_strong_pivots <= cfg.branching.node_budget);
        MESSAGE("a280 strong: candidates=" << res.strong_candidates << " pivots=" << res.strong_pivots
                << " max/node=" << res.max_node_strong_pivots << " nodes=" << res.nodes);
    }
    // Direct call with a tight budget on the root LP; bounds are restored afterwards.
    auto g = apex::build_candidate_graph(inst);
    apex::RootLoopConfig rc;
    apex::CutLoop loop(inst, g, rc);
    const auto r = loop.solve({}, nullptr, apex::NodeLimits{});
    for (uint32_t budget : {1u, 7u, 15u, 60u}) {
        CAPTURE(budget);
        apex::Lp lp = loop.lp();
        const auto lower = lp.lower, upper = lp.upper;
        apex::Pseudocosts pc;
        apex::BranchingConfig bc;
        bc.node_budget = budget;
        const auto d = apex::select_branch(lp, loop.basis().basis, r.final, rc.lp, pc, bc);
        CHECK(d.strong_pivots <= budget);
        CHECK(d.edge >= 0);
        CHECK(lp.lower == lower);
        CHECK(lp.upper == upper);
    }
}
