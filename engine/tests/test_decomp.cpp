#include <doctest/doctest.h>

#include <chrono>
#include <random>
#include <string>

#include "decomp/kdtree.hpp"
#include "decomp/patching.hpp"

namespace {

apex::Instance random_instance(uint32_t n, uint64_t seed, bool clustered) {
    apex::Instance inst;
    inst.name = "rand" + std::to_string(seed);
    inst.n = n;
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(0, 100000);
    std::normal_distribution<double> nd(0, 1500);
    std::vector<std::pair<double, double>> centers;
    for (int k = 0; k < 12; ++k) centers.push_back({u(rng), u(rng)});
    for (uint32_t i = 0; i < n; ++i) {
        if (clustered) {
            const auto& c = centers[rng() % centers.size()];
            inst.xs.push_back(c.first + nd(rng));
            inst.ys.push_back(c.second + nd(rng));
        } else {
            inst.xs.push_back(u(rng));
            inst.ys.push_back(u(rng));
        }
        inst.original_ids.push_back(i + 1);
    }
    return inst;
}

}  // namespace

TEST_CASE("T_kdtree_leaf_sizes") {
    const auto inst = random_instance(85900, 3, false);
    const auto tree = apex::build_kdtree(inst, 800);
    uint32_t lo = UINT32_MAX, hi = 0, total = 0;
    for (const auto& nd : tree)
        if (nd.left < 0) {
            lo = std::min<uint32_t>(lo, static_cast<uint32_t>(nd.points.size()));
            hi = std::max<uint32_t>(hi, static_cast<uint32_t>(nd.points.size()));
            total += static_cast<uint32_t>(nd.points.size());
        }
    CHECK(total == inst.n);
    CHECK(lo >= 500);
    CHECK(hi <= 800);
    MESSAGE("leaf sizes " << lo << ".." << hi);
}

TEST_CASE("T_patch_produces_valid_tour") {
    for (uint64_t s = 1; s <= 20; ++s) {
        CAPTURE(s);
        const auto inst = random_instance(1500 + 250 * static_cast<uint32_t>(s), s, s % 2 == 0);
        const auto g = apex::build_candidate_graph(inst);
        apex::DecompConfig cfg;
        cfg.max_leaf = s <= 2 ? 120 : 400;  // s = 1, 2: leaves <= 300 take the exact mini B&C path
        cfg.leaf.ils_iterations_per_node = 2;
        cfg.leaf.exact_max_nodes = 20;
        cfg.leaf.threads = 4;
        apex::DecompStats st;
        const auto t = apex::decompose_and_patch(inst, g, cfg, &st);
        REQUIRE(apex::is_valid_tour(t, inst.n));
        CHECK(t.cost == apex::tour_cost(inst, t));
        CHECK(st.final_cost == t.cost);
        CHECK(st.final_cost <= st.patched_cost);
        CHECK(st.merges + 1 == st.leaf.leaves);
        if (s <= 2) CHECK(st.leaf.exact_leaves == st.leaf.leaves);
    }
}

TEST_CASE("T_patch_pla85900") {
    const auto inst = apex::read_tsplib(std::string(APEX_DATA_DIR) + "/pla85900.tsp");
    const auto g = apex::build_candidate_graph(inst);
    apex::DecompConfig cfg;
    cfg.leaf.ils_iterations_per_node = 1;
    cfg.leaf.threads = 4;
    apex::DecompStats st;
    const auto t0 = std::chrono::steady_clock::now();
    const auto t = apex::decompose_and_patch(inst, g, cfg, &st);
    REQUIRE(apex::is_valid_tour(t, inst.n));
    CHECK(t.cost == apex::tour_cost(inst, t));
    CHECK(st.leaf.min_size >= 500);
    CHECK(st.leaf.max_size <= 800);
    MESSAGE("pla85900 leaves=" << st.leaf.leaves << " leaf_sum=" << st.leaf_sum << " patched=" << st.patched_cost
                               << " final=" << t.cost << " vs 142382641: "
                               << 100.0 * (t.cost - 142382641.0) / 142382641.0 << "% s="
                               << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

TEST_CASE("T_leaf_parallel_deterministic_option") {
    const auto inst = random_instance(6000, 77, true);
    const auto g = apex::build_candidate_graph(inst);
    apex::DecompConfig cfg;
    cfg.max_leaf = 500;
    cfg.leaf.ils_iterations_per_node = 3;
    cfg.leaf.threads = 1;
    const auto a = apex::decompose_and_patch(inst, g, cfg);
    cfg.leaf.threads = 5;
    const auto b = apex::decompose_and_patch(inst, g, cfg);
    CHECK(a.order == b.order);
    CHECK(a.cost == b.cost);
}
