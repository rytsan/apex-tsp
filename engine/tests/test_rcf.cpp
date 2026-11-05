#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "bnc/rcf.hpp"
#include "bnc/tree.hpp"
#include "geo/hilbert.hpp"
#include "primal/two_opt.hpp"

namespace {

apex::Instance random_instance(uint32_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> c(0, 1000);
    apex::Instance inst;
    inst.name = "rand" + std::to_string(seed);
    inst.n = n;
    for (uint32_t i = 0; i < n; ++i) {
        inst.xs.push_back(c(rng));
        inst.ys.push_back(c(rng));
        inst.original_ids.push_back(i + 1);
    }
    return inst;
}

// Exhaustive enumeration: optimum, and per edge (u < v) the cheapest tour using it / avoiding it.
struct Brute {
    int64_t opt = INT64_MAX;
    std::vector<int64_t> through, avoid;  // index u * n + v
};

Brute brute_force(const apex::Instance& inst) {
    const uint32_t n = inst.n;
    Brute b;
    b.through.assign(n * n, INT64_MAX);
    b.avoid.assign(n * n, INT64_MAX);
    std::vector<uint32_t> p(n);
    std::iota(p.begin(), p.end(), 0u);
    std::vector<uint8_t> in(n * n);
    do {
        int64_t cost = 0;
        std::fill(in.begin(), in.end(), uint8_t{0});
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t a = p[k], c = p[(k + 1) % n];
            cost += apex::dist(inst, a, c);
            in[std::min(a, c) * n + std::max(a, c)] = 1;
        }
        b.opt = std::min(b.opt, cost);
        for (uint32_t u = 0; u < n; ++u)
            for (uint32_t v = u + 1; v < n; ++v) {
                auto& slot = in[u * n + v] ? b.through[u * n + v] : b.avoid[u * n + v];
                slot = std::min(slot, cost);
            }
    } while (std::next_permutation(p.begin() + 1, p.end()));
    return b;
}

// Held-Karp optimum (n <= 16).
int64_t held_karp(const apex::Instance& inst) {
    const uint32_t n = inst.n, full = 1u << (n - 1);
    std::vector<int64_t> dp(static_cast<size_t>(full) * (n - 1), INT64_MAX / 4);
    for (uint32_t j = 0; j + 1 < n; ++j) dp[(1u << j) * (n - 1) + j] = apex::dist(inst, 0, j + 1);
    for (uint32_t S = 1; S < full; ++S)
        for (uint32_t j = 0; j + 1 < n; ++j) {
            if (!(S >> j & 1)) continue;
            const int64_t cur = dp[static_cast<size_t>(S) * (n - 1) + j];
            for (uint32_t k = 0; k + 1 < n; ++k) {
                if (S >> k & 1) continue;
                auto& nx = dp[static_cast<size_t>(S | 1u << k) * (n - 1) + k];
                nx = std::min(nx, cur + apex::dist(inst, j + 1, k + 1));
            }
        }
    int64_t best = INT64_MAX;
    for (uint32_t j = 0; j + 1 < n; ++j) best = std::min(best, dp[static_cast<size_t>(full - 1) * (n - 1) + j] + apex::dist(inst, j + 1, 0));
    return best;
}

constexpr uint32_t kInstances = 20;
uint32_t size_of(uint32_t k) { return 7 + k % 4; }  // n = 7..10

}  // namespace

TEST_CASE("T_rcf_never_prunes_optimal_edge") {
    // For every fixing at UB: the cheapest tour through a 0-fixed edge (avoiding a 1-fixed edge)
    // must cost >= UB, i.e. no tour strictly better than UB is lost. UB = opt + 1 is the spec's
    // property (no optimal-tour edge pruned); UB = opt checks the boundary exactly. A larger UB
    // must fix a subset (the rule is monotone in UB).
    uint64_t total = 0;
    for (uint32_t k = 0; k < kInstances; ++k) {
        const auto inst = random_instance(size_of(k), 1000 + k);
        CAPTURE(inst.name);
        const auto bf = brute_force(inst);
        const uint32_t n = inst.n;
        std::set<std::pair<uint32_t, uint8_t>> prev;
        for (int64_t ub : {bf.opt, bf.opt + 1, bf.opt + 100}) {
            CAPTURE(ub);
            auto g = apex::build_candidate_graph(inst);
            apex::RootLoopConfig cfg;
            const auto r = apex::run_root_loop(inst, g, cfg);
            REQUIRE(r.converged);
            REQUIRE(r.global_lb <= bf.opt + 1e-6);
            std::set<std::pair<uint32_t, uint8_t>> fixed;  // (u * n + v, value)
            for (const auto& f : apex::rcf_fixings(r.final, r.global_lb, ub)) {
                const auto& e = g.edges[f.edge];
                const uint32_t idx = e.u * n + e.v;
                CHECK((f.value == 0 ? bf.through[idx] : bf.avoid[idx]) >= ub);
                fixed.insert({idx, f.value});
            }
            if (ub > bf.opt) CHECK(std::includes(prev.begin(), prev.end(), fixed.begin(), fixed.end()));
            if (ub == bf.opt) total += fixed.size();
            prev = fixed;
        }
    }
    MESSAGE("instances=" << kInstances << " fixings at UB=opt: " << total);
    CHECK(total > 0);
}

TEST_CASE("T_bnc_matches_exact_optimum") {
    // End to end (root + node RCF, reliability branching) from a poor initial tour, against the
    // exhaustive / Held-Karp optimum. The second half (n = 12..16, no blossoms, one cut round per
    // node) forces branching so node-scoped RCF and child bounds are exercised.
    uint64_t nodes = 0, node_fixes = 0, branched = 0;
    for (uint32_t k = 0; k < 2 * kInstances; ++k) {
        const bool forced = k >= kInstances;
        const auto inst = forced ? random_instance(12 + k % 5, 5000 + k) : random_instance(size_of(k), 1000 + k);
        CAPTURE(inst.name);
        const int64_t opt = forced ? held_karp(inst) : brute_force(inst).opt;
        auto g = apex::build_candidate_graph(inst);
        apex::BnCConfig cfg;
        if (forced) {
            cfg.root.use_blossom = false;
            cfg.root.max_rounds = 1;
            cfg.node.max_rounds = 1;
        }
        apex::Tour t;  // identity tour (poor UB); forced half: 2-opt tour (tight UB => node RCF fires)
        t.order.resize(inst.n);
        std::iota(t.order.begin(), t.order.end(), 0u);
        if (forced) {
            t = apex::hilbert_tour(inst, apex::hilbert_order(inst));
            apex::two_opt_oropt(t, inst, g, apex::TwoOptConfig{});
        }
        const auto res = apex::run_bnc(inst, g, cfg, t);
        CHECK(res.status == apex::BnCResult::Status::Optimal);
        CHECK(res.ub == opt);
        CHECK(res.lb_int == opt);
        CHECK(apex::tour_cost(inst, res.best) == opt);
        nodes += res.nodes;
        node_fixes += res.rcf_node_fixings;
        branched += res.branched;
    }
    MESSAGE("nodes=" << nodes << " branched=" << branched << " node_rcf_fixings=" << node_fixes);
    CHECK(branched > 0);
}
