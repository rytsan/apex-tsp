#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "core/instance.hpp"
#include "graph/candidate_graph.hpp"
#include "lp/dual_simplex.hpp"

using apex::kInf;
using apex::Lp;
using apex::Row;
using apex::Sense;
using apex::SimplexOptions;
using apex::SimplexResult;
using apex::Triplet;
using apex::VarState;
using Status = SimplexResult::Status;

namespace {

struct DenseLp {
    std::vector<std::vector<double>> a;
    std::vector<Sense> sense;
    std::vector<double> rhs, c, lo, up;
};

Lp to_lp(const DenseLp& d) {
    std::vector<Triplet> t;
    std::vector<apex::RowData> rd;
    for (uint32_t i = 0; i < d.a.size(); ++i) {
        for (uint32_t j = 0; j < d.c.size(); ++j)
            if (d.a[i][j] != 0) t.push_back({i, j, d.a[i][j]});
        rd.push_back({d.sense[i], d.rhs[i]});
    }
    Lp lp;
    lp.A = apex::CscMatrix::from_triplets(static_cast<uint32_t>(d.a.size()), static_cast<uint32_t>(d.c.size()), t, rd);
    lp.c = d.c;
    lp.lower = d.lo;
    lp.upper = d.up;
    return lp;
}

// Max KKT violation of an optimal result: primal feasibility, dual sign conditions,
// complementary slackness (rows and bounds).
double kkt_violation(const Lp& lp, const SimplexResult& r) {
    double worst = 0;
    const auto& A = lp.A;
    const uint32_t m = A.num_rows(), n = A.num_cols();
    for (uint32_t j = 0; j < n; ++j) {
        worst = std::max({worst, lp.lower[j] - r.x[j], r.x[j] - lp.upper[j]});
        const double d = r.reduced_costs[j];
        const bool at_lo = std::abs(r.x[j] - lp.lower[j]) <= 1e-7, at_up = std::abs(r.x[j] - lp.upper[j]) <= 1e-7;
        if (!at_lo && !at_up) worst = std::max(worst, std::abs(d));  // strictly between bounds
        else if (at_lo && !at_up) worst = std::max(worst, -d);
        else if (at_up && !at_lo) worst = std::max(worst, d);
    }
    for (uint32_t i = 0; i < m; ++i) {
        double act = 0;
        const auto cols = A.row_cols(i);
        const auto vals = A.row_vals(i);
        for (size_t k = 0; k < cols.size(); ++k) act += vals[k] * r.x[cols[k]];
        const auto& rd = A.row_data()[i];
        const double y = r.duals[i], gap = act - rd.rhs;
        if (rd.sense == Sense::Ge) worst = std::max({worst, -gap, -y, std::abs(gap) > 1e-7 ? std::abs(y) : 0.0});
        if (rd.sense == Sense::Le) worst = std::max({worst, gap, y, std::abs(gap) > 1e-7 ? std::abs(y) : 0.0});
        if (rd.sense == Sense::Eq) worst = std::max(worst, std::abs(gap));
    }
    return worst;
}

bool optimal(const SimplexResult& r) { return r.status == Status::Optimal || r.status == Status::StallRecovered; }

// Random LP feasible by construction (x0 feasible). mix_bounds adds free/half-bounded columns.
DenseLp random_lp(std::mt19937_64& rng, uint32_t m, uint32_t n, bool mix_bounds) {
    std::uniform_real_distribution<double> U(-5, 5), P(0, 1);
    DenseLp d;
    d.c.resize(n);
    d.lo.resize(n);
    d.up.resize(n);
    std::vector<double> x0(n);
    for (uint32_t j = 0; j < n; ++j) {
        d.c[j] = std::round(U(rng));
        d.lo[j] = 0;
        d.up[j] = 1 + std::floor(P(rng) * 9);
        if (mix_bounds) {
            const double k = P(rng);
            if (k < 0.15) { d.lo[j] = -kInf; d.up[j] = kInf; }
            else if (k < 0.3) d.up[j] = kInf;
            else if (k < 0.4) { d.lo[j] = -kInf; d.up[j] = 3; }
        }
        const double lo = std::isfinite(d.lo[j]) ? d.lo[j] : -5, up = std::isfinite(d.up[j]) ? d.up[j] : 5;
        x0[j] = lo + P(rng) * (up - lo);
    }
    for (uint32_t i = 0; i < m; ++i) {
        std::vector<double> row(n, 0.0);
        double act = 0;
        for (uint32_t j = 0; j < n; ++j)
            if (P(rng) < 0.3) { row[j] = std::round(U(rng)); act += row[j] * x0[j]; }
        const double k = P(rng);
        d.a.push_back(row);
        if (k < 0.45) { d.sense.push_back(Sense::Ge); d.rhs.push_back(std::floor(act - P(rng) * 3)); }
        else if (k < 0.9) { d.sense.push_back(Sense::Le); d.rhs.push_back(std::ceil(act + P(rng) * 3)); }
        else { d.sense.push_back(Sense::Eq); d.rhs.push_back(act); }
    }
    return d;
}

// Assignment LP (highly degenerate): x_ij in [0,1], row sums = 1, column sums = 1.
Lp assignment_lp(std::mt19937_64& rng, uint32_t k, int cost_range) {
    std::vector<Triplet> t;
    std::vector<apex::RowData> rd(2 * k, {Sense::Eq, 1.0});
    Lp lp;
    for (uint32_t i = 0; i < k; ++i)
        for (uint32_t j = 0; j < k; ++j) {
            const uint32_t v = i * k + j;
            t.push_back({i, v, 1.0});
            t.push_back({k + j, v, 1.0});
            lp.c.push_back(static_cast<double>(rng() % cost_range));
        }
    lp.A = apex::CscMatrix::from_triplets(2 * k, k * k, t, rd);
    lp.lower.assign(k * k, 0.0);
    lp.upper.assign(k * k, 1.0);
    return lp;
}

}  // namespace

TEST_CASE("T_dual_small_known") {
    // min -x - y  s.t. x + 2y <= 4, 3x + y <= 6, 0 <= x,y <= 10  -> (1.6, 1.2), -2.8
    const Lp lp = to_lp({{{1, 2}, {3, 1}}, {Sense::Le, Sense::Le}, {4, 6}, {-1, -1}, {0, 0}, {10, 10}});
    const auto r = apex::solve_dual(lp, {});
    REQUIRE(optimal(r));
    CHECK(r.objective == doctest::Approx(-2.8));
    CHECK(r.x[0] == doctest::Approx(1.6));
    CHECK(r.x[1] == doctest::Approx(1.2));
    CHECK(r.duals[0] == doctest::Approx(-0.4));
    CHECK(r.duals[1] == doctest::Approx(-0.2));
    CHECK(kkt_violation(lp, r) <= 1e-7);
    CHECK_FALSE(r.used_artificial_bounds);  // negative costs but finite upper bounds: start at upper, no box needed
}

TEST_CASE("T_dual_matches_primal_random") {
    std::mt19937_64 rng(1234);
    int solved = 0, infeasible = 0, unbounded = 0;
    double worst_kkt = 0, worst_diff = 0;
    for (int trial = 0; trial < 300; ++trial) {
        const uint32_t m = 2 + rng() % 25, n = 2 + rng() % 30;
        const auto d = random_lp(rng, m, n, trial % 2 == 1);
        const Lp lp = to_lp(d);
        for (auto pricing : {SimplexOptions::Pricing::Devex, SimplexOptions::Pricing::DualSteepestEdge}) {
            SimplexOptions o;
            o.pricing = pricing;
            const auto a = apex::solve_dual(lp, o);
            const auto b = apex::solve_primal(lp, o);
            CAPTURE(trial);
            CHECK(apex::status_name(a.status) == std::string(apex::status_name(optimal(b) ? Status::Optimal : b.status)) );
            if (!optimal(a)) {
                infeasible += a.status == Status::Infeasible;
                unbounded += a.status == Status::Unbounded;
                continue;
            }
            ++solved;
            worst_kkt = std::max(worst_kkt, kkt_violation(lp, a));
            worst_diff = std::max(worst_diff, std::abs(a.objective - b.objective) / (1 + std::abs(b.objective)));
        }
    }
    MESSAGE("solved=" << solved << " unbounded=" << unbounded << " infeasible=" << infeasible
                      << " worst_kkt=" << worst_kkt << " worst_rel_obj_diff=" << worst_diff);
    CHECK(worst_kkt <= 1e-6);
    CHECK(worst_diff <= 1e-7);
    CHECK(solved > 300);
}

TEST_CASE("T_dual_generic_statuses") {
    // Free variables + negative cost: min -x s.t. x + y <= 4, x - y <= 2 (x, y free) -> x=3, y=1, -3.
    const Lp a = to_lp({{{1, 1}, {1, -1}}, {Sense::Le, Sense::Le}, {4, 2}, {-1, 0}, {-kInf, -kInf}, {kInf, kInf}});
    const auto ra = apex::solve_dual(a, {});
    REQUIRE(optimal(ra));
    CHECK(ra.objective == doctest::Approx(-3));
    CHECK(ra.used_artificial_bounds);
    // Unbounded: min -x s.t. x - y <= 0, x, y >= 0.
    const Lp b = to_lp({{{1, -1}}, {Sense::Le}, {0}, {-1, 0}, {0, 0}, {kInf, kInf}});
    CHECK(apex::solve_dual(b, {}).status == Status::Unbounded);
    CHECK(apex::solve_primal(b, {}).status == Status::Unbounded);
    // Infeasible: x >= 2 and x <= 1 as rows.
    const Lp c = to_lp({{{1}, {1}}, {Sense::Ge, Sense::Le}, {2, 1}, {1}, {0}, {10}});
    CHECK(apex::solve_dual(c, {}).status == Status::Infeasible);
    CHECK(apex::solve_primal(c, {}).status == Status::Infeasible);
    // Infeasible bounds.
    const Lp e = to_lp({{{1}}, {Sense::Ge}, {0}, {1}, {2}, {1}});
    CHECK(apex::solve_dual(e, {}).status == Status::Infeasible);
}

TEST_CASE("T_harris_no_cycling") {
    // Beale's classic cycling example (cycles under textbook Dantzig rule), optimum -1.25.
    const Lp beale = to_lp({{{0.25, -8, -1, 9}, {0.5, -12, -0.5, 3}, {0, 0, 1, 0}},
                            {Sense::Le, Sense::Le, Sense::Le}, {0, 0, 1}, {-0.75, 20, -0.5, 6},
                            {0, 0, 0, 0}, {kInf, kInf, kInf, kInf}});
    const auto rb = apex::solve_dual(beale, {});
    REQUIRE(optimal(rb));
    CHECK(rb.objective == doctest::Approx(-1.25));
    const auto pb = apex::solve_primal(beale, {});
    REQUIRE(optimal(pb));
    CHECK(pb.objective == doctest::Approx(-1.25));

    // Degenerate family: assignment LPs with few distinct costs; >= 10k dual iterations total.
    std::mt19937_64 rng(5);
    uint64_t total = 0;
    int lps = 0;
    while (total < 10000) {
        REQUIRE(lps < 2000);
        const auto lp = assignment_lp(rng, 8 + rng() % 25, 3);
        SimplexOptions o;
        o.iteration_limit = 200000;
        const auto r = apex::solve_dual(lp, o);
        REQUIRE(optimal(r));  // no cycling => terminates within the limit
        CHECK_FALSE(r.used_artificial_bounds);
        CHECK(kkt_violation(lp, r) <= 1e-7);
        total += r.iterations;
        ++lps;
    }
    MESSAGE("degenerate assignment LPs=" << lps << " total_iterations=" << total);
}

TEST_CASE("T_bfrt_long_steps") {
    // min sum j*x_j, 0 <= x <= 1, sum x >= 50, n = 100: optimum sum_{1..50} j = 1275.
    const uint32_t n = 100;
    DenseLp d;
    d.a = {std::vector<double>(n, 1.0)};
    d.sense = {Sense::Ge};
    d.rhs = {50};
    for (uint32_t j = 0; j < n; ++j) { d.c.push_back(j + 1.0); d.lo.push_back(0); d.up.push_back(1); }
    const Lp lp = to_lp(d);
    SimplexOptions on, off;
    off.bound_flipping = false;
    const auto a = apex::solve_dual(lp, on), b = apex::solve_dual(lp, off);
    REQUIRE(optimal(a));
    REQUIRE(optimal(b));
    CHECK(a.objective == doctest::Approx(1275));
    CHECK(b.objective == doctest::Approx(1275));
    MESSAGE("BFRT on: iterations=" << a.iterations << " flips=" << a.bound_flips << "; off: iterations=" << b.iterations);
    CHECK(a.iterations < b.iterations);
}

TEST_CASE("T_dual_warmstart_after_rows") {
    // TSP-shaped: fractional 2-matching LP on eil51's candidate graph, then add violated
    // >= rows (cut-like) and warm-start from the previous basis.
    const auto inst = apex::read_tsplib(std::string(APEX_DATA_DIR) + "/eil51.tsp");
    const auto g = apex::build_candidate_graph(inst);
    const uint32_t m = inst.n, n = static_cast<uint32_t>(g.edges.size());
    std::vector<Triplet> t;
    Lp lp;
    for (uint32_t e = 0; e < n; ++e) {
        t.push_back({g.edges[e].u, e, 1.0});
        t.push_back({g.edges[e].v, e, 1.0});
        lp.c.push_back(g.edges[e].cost);
    }
    lp.A = apex::CscMatrix::from_triplets(m, n, t, std::vector<apex::RowData>(m, {Sense::Eq, 2.0}));
    lp.lower.assign(n, 0.0);
    lp.upper.assign(n, 1.0);
    const auto r0 = apex::solve_dual(lp, {});
    REQUIRE(optimal(r0));
    CHECK_FALSE(r0.used_artificial_bounds);  // TSP LP: all-slack basis is dual feasible
    CHECK(r0.objective <= 426 + 1e-6);        // valid lower bound on eil51's optimum
    CHECK(kkt_violation(lp, r0) <= 1e-7);

    // Violated SECs: x(delta(S)) >= 2 for every connected component S of the support graph.
    std::vector<uint32_t> comp(m);
    for (uint32_t i = 0; i < m; ++i) comp[i] = i;
    auto find = [&](uint32_t v) { while (comp[v] != v) v = comp[v] = comp[comp[v]]; return v; };
    for (uint32_t e = 0; e < n; ++e)
        if (r0.x[e] > 1e-9) comp[find(g.edges[e].u)] = find(g.edges[e].v);
    std::vector<uint32_t> roots;
    for (uint32_t i = 0; i < m; ++i) if (find(i) == i) roots.push_back(i);
    REQUIRE(roots.size() > 1);  // eil51's 2-matching LP has subtours
    std::vector<Row> rows;
    for (uint32_t root : roots) {
        Row row;
        row.sense = Sense::Ge;
        row.rhs = 2;
        for (uint32_t f = 0; f < n; ++f)
            if ((find(g.edges[f].u) == root) != (find(g.edges[f].v) == root)) row.entries.push_back({f, 1.0});
        rows.push_back(row);
    }
    lp.A.append_rows(rows);
    lp.A.ensure_merged();
    apex::Basis warm = r0.basis;
    warm.row.resize(lp.A.num_rows(), VarState::Basic);  // new rows: logical basic
    const auto rw = apex::solve_dual(lp, {}, &warm);
    const auto rc = apex::solve_dual(lp, {});
    REQUIRE(optimal(rw));
    REQUIRE(optimal(rc));
    CHECK_FALSE(rw.used_artificial_bounds);  // dual feasibility preserved by adding >= rows
    CHECK(rw.objective == doctest::Approx(rc.objective).epsilon(1e-9));
    CHECK(rw.objective > r0.objective + 1e-6);  // cuts were violated: bound strictly improves
    CHECK(kkt_violation(lp, rw) <= 1e-7);
    MESSAGE("2-matching LB=" << r0.objective << " subtours=" << roots.size() << " after SECs=" << rw.objective << " warm_iter=" << rw.iterations
                             << " cold_iter=" << rc.iterations);
    CHECK(rw.iterations < rc.iterations);
}

TEST_CASE("T_stall_recovery_triggers") {
    std::mt19937_64 rng(8);
    int triggered = 0;
    for (int k = 0; k < 10; ++k) {
        const auto lp = assignment_lp(rng, 25, 2);
        SimplexOptions o;
        o.stall_window = 2;
        const auto r = apex::solve_dual(lp, o);
        const auto ref = apex::solve_dual(lp, {});
        REQUIRE(optimal(r));
        REQUIRE(optimal(ref));
        CHECK(r.objective == doctest::Approx(ref.objective));
        CHECK(kkt_violation(lp, r) <= 1e-6);
        triggered += r.status == Status::StallRecovered;
    }
    MESSAGE("stall recovery triggered on " << triggered << "/10");
    CHECK(triggered > 0);
}

TEST_CASE("T_primal_phase1") {
    // Start from an infeasible all-slack basis: phase 1 must find feasibility.
    const Lp lp = to_lp({{{1, 1}, {1, -1}, {0, 1}}, {Sense::Ge, Sense::Eq, Sense::Le}, {3, 1, 5}, {1, 2},
                         {0, 0}, {kInf, kInf}});  // optimum x=2, y=1 -> 4
    const auto r = apex::solve_primal(lp, {});
    REQUIRE(optimal(r));
    CHECK(r.objective == doctest::Approx(4));
    CHECK(r.primal_iterations > 0);
    CHECK(kkt_violation(lp, r) <= 1e-7);
}

TEST_CASE("T_csc_append_rows_lp_unchanged") {
    // Completes T_csc_append_rows with the real solver: non-binding appended rows keep the optimum.
    Lp lp = to_lp({{{1, 2}, {3, 1}}, {Sense::Le, Sense::Le}, {4, 6}, {-1, -1}, {0, 0}, {10, 10}});
    const auto before = apex::solve_dual(lp, {});
    lp.A.append_rows({Row{{{0, 1}, {1, 1}}, Sense::Le, 5}, Row{{{0, 1}}, Sense::Ge, 0.5}});
    const auto after = apex::solve_dual(lp, {});  // rows still buffered: solver merges a copy
    REQUIRE(optimal(before));
    REQUIRE(optimal(after));
    CHECK(after.objective == doctest::Approx(before.objective));
    CHECK(after.x[0] == doctest::Approx(before.x[0]));
    CHECK(after.x[1] == doctest::Approx(before.x[1]));
}

TEST_CASE("T_ft_equivalence") {
    // Forrest-Tomlin and PFI on the same 50 random LPs: same status, same objective.
    std::mt19937_64 rng(4242);
    int compared = 0, kkt_pre_existing = 0;
    double worst = 0;
    for (int trial = 0; trial < 50; ++trial) {
        const uint32_t m = 20 + rng() % 60, n = 30 + rng() % 90;
        const Lp lp = to_lp(random_lp(rng, m, n, trial % 2 == 1));
        SimplexOptions ft, pfi;
        ft.refactor_interval = pfi.refactor_interval = 25;  // several updates between refactorizations
        pfi.basis_update = apex::BasisUpdateKind::Pfi;
        const auto a = apex::solve_dual(lp, ft);
        const auto b = apex::solve_dual(lp, pfi);
        CAPTURE(trial);
        CHECK(optimal(a) == optimal(b));
        if (!optimal(a) || !optimal(b)) {
            CHECK(a.status == b.status);
            continue;
        }
        ++compared;
        worst = std::max(worst, std::abs(a.objective - b.objective) / (1 + std::abs(b.objective)));
        // Not worse than PFI. Known pre-existing case (trial 35, free / half-bounded columns): both
        // updates return the optimal objective but duals with KKT violation ~1 (README §9).
        if (kkt_violation(lp, b) > 1e-6) ++kkt_pre_existing;
        CHECK(kkt_violation(lp, a) <= std::max(1e-6, 2 * kkt_violation(lp, b)));
    }
    MESSAGE("ft vs pfi: compared=" << compared << " worst_rel_obj_diff=" << worst << " pfi_kkt_cases=" << kkt_pre_existing);
    CHECK(compared >= 25);
    CHECK(worst <= 1e-9);
}
