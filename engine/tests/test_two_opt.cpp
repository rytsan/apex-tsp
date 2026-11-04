#include <doctest/doctest.h>

#include <chrono>
#include <string>

#include "core/instance.hpp"
#include "geo/hilbert.hpp"
#include "graph/candidate_graph.hpp"
#include "graph/tours.hpp"
#include "primal/two_opt.hpp"

namespace {

struct Run {
    apex::Tour hilbert, opt;
    apex::TwoOptStats st;
    double ms;
};

Run run(const apex::Instance& in, const apex::TwoOptConfig& cfg) {
    const auto g = apex::build_candidate_graph(in);
    Run r;
    r.hilbert = apex::hilbert_tour(in, apex::hilbert_order(in));
    r.opt = r.hilbert;
    const auto t0 = std::chrono::steady_clock::now();
    r.st = apex::two_opt_oropt(r.opt, in, g, cfg);
    r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

apex::Instance load(const std::string& name) {
    return apex::read_tsplib(std::string(APEX_DATA_DIR) + "/" + name + ".tsp");
}

}  // namespace

TEST_CASE("T_two_opt_improves_hilbert") {
    struct Ref { const char* name; int64_t opt; };  // MASTER §7
    const Ref refs[] = {{"eil51", 426}, {"berlin52", 7542}, {"a280", 2579}, {"pr1002", 259045},
                        {"fl3795", 28772}, {"usa13509", 19982859}};
    for (const auto& r : refs) {
        CAPTURE(r.name);
        const auto in = load(r.name);
        const auto res = run(in, {});
        const double gap0 = 100.0 * (res.hilbert.cost - r.opt) / r.opt;
        const double gap1 = 100.0 * (res.opt.cost - r.opt) / r.opt;
        MESSAGE(std::string(r.name) << " hilbert=" << res.hilbert.cost << " (" << gap0 << "%) 2opt+oropt="
                                    << res.opt.cost << " (" << gap1 << "%) moves2=" << res.st.moves_2opt
                                    << " movesOr=" << res.st.moves_oropt << " rounds=" << res.st.rounds
                                    << " K=" << res.st.final_neighbors << " ms=" << res.ms);
        CHECK(apex::is_valid_tour(res.opt, in.n));
        CHECK(res.opt.cost == apex::tour_cost(in, res.opt));  // incremental cost bookkeeping is exact
        CHECK(res.opt.cost < res.hilbert.cost);              // gap strictly decreases
        CHECK(res.opt.cost >= r.opt);                         // never below the proven optimum
    }
}

TEST_CASE("T_two_opt_deterministic") {
    const auto in = load("pr1002");
    apex::TwoOptConfig cfg;
    cfg.seed = 7;
    const auto a = run(in, cfg), b = run(in, cfg);
    CHECK(a.opt.order == b.opt.order);
    CHECK(a.opt.cost == b.opt.cost);
    cfg.seed = 8;
    const auto c = run(in, cfg);
    CHECK(apex::is_valid_tour(c.opt, in.n));
    MESSAGE("seed7=" << a.opt.cost << " seed8=" << c.opt.cost);
}

TEST_CASE("T_two_opt_small_and_flags") {
    // Tiny instances: no crash, valid tour, cost bookkeeping exact, 2-opt-only mode works.
    for (uint32_t n : {3u, 4u, 5u, 6u, 9u, 12u}) {
        apex::Instance in;
        in.name = "ring";
        in.n = n;
        for (uint32_t i = 0; i < n; ++i) {
            in.xs.push_back((i * 7919) % 101);
            in.ys.push_back((i * 104729) % 97);
            in.original_ids.push_back(i + 1);
        }
        for (bool oropt : {true, false}) {
            apex::TwoOptConfig cfg;
            cfg.or_opt = oropt;
            const auto r = run(in, cfg);
            CHECK(apex::is_valid_tour(r.opt, n));
            CHECK(r.opt.cost == apex::tour_cost(in, r.opt));
            CHECK(r.opt.cost <= r.hilbert.cost);
        }
    }
}
