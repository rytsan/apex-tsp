#include <doctest/doctest.h>

#include <string>

#include "bnc/root_loop.hpp"
#include "hk/held_karp.hpp"

namespace {

apex::Instance load(const std::string& name) {
    return apex::read_tsplib(std::string(APEX_DATA_DIR) + "/" + name + ".tsp");
}

}  // namespace

TEST_CASE("T_hk_bound_valid") {
    struct Ref { const char* name; int64_t opt; };
    for (const auto& r : {Ref{"eil51", 426}, Ref{"berlin52", 7542}, Ref{"a280", 2579}, Ref{"pr1002", 259045}}) {
        CAPTURE(r.name);
        const auto inst = load(r.name);
        const auto g = apex::build_candidate_graph(inst);
        const auto hk = apex::held_karp(inst, g, r.opt, apex::HkConfig{});
        REQUIRE(hk.valid);
        CHECK(hk.lb <= r.opt + 1e-6);
        CHECK(hk.lb_int <= r.opt);
        CHECK(hk.lb > 0.98 * r.opt);
        // pi = 0: plain minimum 1-tree, also a bound, never above the optimized one by much.
        const double plain = apex::one_tree_bound_complete(inst, std::vector<double>(inst.n, 0.0));
        CHECK(plain <= r.opt);
        CHECK(hk.lb >= plain);
        MESSAGE(std::string(r.name) << " hk=" << hk.lb << " (" << 100.0 * hk.lb / r.opt << "% of opt) cand=" << hk.lb_candidate
                                    << " plain_1tree=" << plain << " iterations=" << hk.iterations
                                    << " ascent_s=" << hk.ascent_s << " exact_s=" << hk.exact_s);
    }
}

TEST_CASE("T_hk_convergence") {
    // HK = subtour LP at the optimal pi; the subgradient ascent gets within 0.5% of it on pr1002.
    const auto inst = load("pr1002");
    auto g = apex::build_candidate_graph(inst);
    const auto hk = apex::held_karp(inst, g, 259045, apex::HkConfig{});
    apex::RootLoopConfig cfg;
    cfg.use_blossom = false;
    apex::CutLoop loop(inst, g, cfg);
    const auto r = loop.solve({}, nullptr, apex::NodeLimits{});
    REQUIRE(r.converged);
    const double subtour = r.global_lb;
    CHECK(hk.lb <= subtour + 1e-6);  // cross-check (c): above the subtour LP would be a bug
    CHECK(hk.lb >= 0.995 * subtour);
    MESSAGE("pr1002 subtour_lp=" << subtour << " hk=" << hk.lb << " ratio=" << 100.0 * hk.lb / subtour << "%");
}
