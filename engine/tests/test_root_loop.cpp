#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "bnc/root_loop.hpp"
#include "core/tour_io.hpp"

namespace {
apex::Instance load(const std::string& name) {
    return apex::read_tsplib(std::string(APEX_DATA_DIR) + "/" + name + ".tsp");
}
}  // namespace

TEST_CASE("T_root_bound_honest_and_monotone") {
    struct Ref { const char* name; double opt; };
    for (const auto& r : {Ref{"eil51", 426}, Ref{"berlin52", 7542}, Ref{"a280", 2579}}) {
        CAPTURE(r.name);
        const auto inst = load(r.name);
        auto g = apex::build_candidate_graph(inst);
        apex::RootLoopConfig cfg;
        const auto res = apex::run_root_loop(inst, g, cfg);
        CHECK(res.converged);
        CHECK(res.lp_value <= r.opt + 1e-6);          // above the optimum = bug
        CHECK(res.global_lb <= res.lp_value + 1e-6);  // Farley bound never exceeds the LP value
        CHECK(res.global_lb <= r.opt + 1e-6);
        CHECK(res.lp_value >= 0.97 * r.opt);
        for (size_t k = 1; k < res.rounds.size(); ++k)  // non-decreasing across re-solves
            CHECK(res.rounds[k].lp_value >= res.rounds[k - 1].lp_value - 1e-6 * (1 + res.rounds[k - 1].lp_value));
        // At convergence no SEC/blossom separator finds anything on the final x.
        const auto sup = apex::build_support(inst.n, g.edges, res.final.x);
        CHECK(apex::separate_sec(sup).empty());
        CHECK(apex::connected_components(sup).size() == 1);
        MESSAGE(std::string(r.name) << " root=" << res.lp_value << " global=" << res.global_lb << " rounds=" << res.rounds.size());
    }
}

TEST_CASE("T_root_rcf_never_prunes_optimal_edge") {
    // UB = optimum + 1: fixing x_e = 0 means every tour through e costs >= opt + 1, so no edge of
    // the published optimal tour may be fixed to 0, and every edge fixed to 1 must be in it.
    struct Ref { const char* name; int64_t opt; };
    for (const auto& r : {Ref{"eil51", 426}, Ref{"berlin52", 7542}, Ref{"a280", 2579}, Ref{"pr1002", 259045}}) {
        CAPTURE(r.name);
        const auto inst = load(r.name);
        const auto opt = apex::read_tour(std::string(APEX_DATA_DIR) + "/" + r.name + ".opt.tour", inst);
        REQUIRE(opt.cost == r.opt);
        std::set<std::pair<uint32_t, uint32_t>> opt_edges;
        for (size_t k = 0; k < opt.order.size(); ++k) {
            const uint32_t a = opt.order[k], b = opt.order[(k + 1) % opt.order.size()];
            opt_edges.insert({std::min(a, b), std::max(a, b)});
        }
        auto g = apex::build_candidate_graph(inst);
        apex::RootLoopConfig cfg;
        cfg.ub = r.opt + 1;
        const auto res = apex::run_root_loop(inst, g, cfg);
        REQUIRE(res.converged);
        uint32_t bad = 0, f0 = 0, f1 = 0;
        for (const auto& e : g.edges) {
            const bool in_opt = opt_edges.count({e.u, e.v}) > 0;
            if (e.status & apex::kFixed0) { ++f0; bad += in_opt; }
            if (e.status & apex::kFixed1) { ++f1; bad += !in_opt; }
        }
        MESSAGE(std::string(r.name) << " lb_int=" << res.global_lb_int << " fixed0=" << f0 << " fixed1=" << f1 << " of " << g.edges.size());
        CHECK(bad == 0);
        CHECK(f0 == res.rcf_fixed0);
        CHECK(f0 > 0);  // with a near-optimal UB the root must fix something
    }
}

TEST_CASE("T_root_rcf_integral_rule") {
    // Documents the MASTER §5.5 correction: z = 10.1, d = 0.6, UB = 12. The spec rule
    // ceil(z) + d > UB - 0.5 (11.6 > 11.5) would fix, yet a tour through e may cost
    // ceil(z + d) = 11 < UB. The sound rule ceil(z + d - 1e-6) >= UB does not fix.
    const double z = 10.1, d = 0.6, ub = 12;
    CHECK(std::ceil(z - 1e-6) + d > ub - 0.5);       // unsound spec rule fires
    CHECK_FALSE(std::ceil(z + d - 1e-6) >= ub);      // sound rule does not
}
