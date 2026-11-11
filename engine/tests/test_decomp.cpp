#include <mutex>
#include <doctest/doctest.h>

#include <chrono>
#include <random>
#include <string>

#include "decomp/kdtree.hpp"
#include "decomp/patching.hpp"
#include "decomp/pipeline.hpp"
#include "primal/lk_lite.hpp"

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
        cfg.leaf.exact_limit = s <= 2 ? 300 : 0;
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
    const auto t1 = std::chrono::steady_clock::now();
    apex::DecompStats sk;
    const auto k = apex::cluster_merge(inst, g, cfg, &sk);
    REQUIRE(apex::is_valid_tour(k, inst.n));
    MESSAGE("pla85900 kway rounds=" << sk.levels << " final=" << k.cost << " vs 142382641: "
                                    << 100.0 * (k.cost - 142382641.0) / 142382641.0 << "% s="
                                    << std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count());
    CHECK(st.leaf.min_size > 800 * 35 / 100);  // density cuts: (0.35 * leaf, leaf]
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
    cfg.leaf.exact_limit = 0;
    cfg.leaf.threads = 1;
    const auto a = apex::decompose_and_patch(inst, g, cfg);
    cfg.leaf.threads = 5;
    const auto b = apex::decompose_and_patch(inst, g, cfg);
    CHECK(a.order == b.order);
    CHECK(a.cost == b.cost);
}

TEST_CASE("T_density_tree_cuts_gaps") {
    // Clustered instance: density cuts (widest gap within the 35-65% ranks) keep every leaf
    // within (0.35 * leaf, leaf] and cover all nodes.
    const auto inst = random_instance(20000, 9, true);
    const auto tree = apex::build_density_tree(inst, 800);
    uint32_t lo = UINT32_MAX, hi = 0, total = 0;
    for (const auto& nd : tree)
        if (nd.left < 0) {
            lo = std::min<uint32_t>(lo, static_cast<uint32_t>(nd.points.size()));
            hi = std::max<uint32_t>(hi, static_cast<uint32_t>(nd.points.size()));
            total += static_cast<uint32_t>(nd.points.size());
        }
    CHECK(total == inst.n);
    CHECK(hi <= 800);
    CHECK(lo > 800 * 35 / 100);
    MESSAGE("density leaves " << lo << ".." << hi << " count=" << (tree.size() + 1) / 2);
}

TEST_CASE("T_fixed_edge_kept") {
    const auto inst = random_instance(3000, 4, false);
    const auto g = apex::build_candidate_graph(inst);
    apex::Tour t;
    for (uint32_t k = 0; k < inst.n; ++k) t.order.push_back(k);  // terrible tour: many moves
    t.cost = apex::tour_cost(inst, t);
    apex::IlsConfig ic;
    ic.max_iterations = 2000;
    ic.fixed_u = 17;
    ic.fixed_v = 18;  // consecutive in the start tour
    apex::iterated_local_search(t, inst, g, ic);
    REQUIRE(apex::is_valid_tour(t, inst.n));
    uint32_t p = 0;
    while (t.order[p] != 17) ++p;
    const uint32_t a = t.order[(p + 1) % inst.n], b = t.order[(p + inst.n - 1) % inst.n];
    CHECK((a == 18 || b == 18));
}

// k-way MST merge: a valid tour, deterministic for any thread count, every intermediate forest
// a set of disjoint cycles, and no worse than the binary density-tree merge by more than 1%.
TEST_CASE("T_cluster_merge_kway") {
    const auto inst = random_instance(20000, 11, true);
    const auto g = apex::build_candidate_graph(inst);
    apex::DecompConfig cfg;
    cfg.leaf.ils_iterations_per_node = 1;
    cfg.leaf.exact_limit = 0;
    cfg.leaf.threads = 4;
    std::mutex mu;
    size_t frames = 0;
    bool disjoint = true;
    cfg.on_forest = [&](const std::vector<std::vector<uint32_t>>& f) {
        std::lock_guard<std::mutex> lk(mu);
        ++frames;
        std::vector<uint8_t> seen(inst.n, 0);
        for (const auto& c : f)
            for (uint32_t v : c) disjoint = disjoint && !seen[v]++;
    };
    apex::DecompStats st;
    const auto a = apex::cluster_merge(inst, g, cfg, &st);
    REQUIRE(apex::is_valid_tour(a, inst.n));
    CHECK(a.cost == apex::tour_cost(inst, a));
    CHECK(disjoint);
    CHECK(frames > st.leaf.leaves);
    CHECK(st.merges + 1 == st.leaf.leaves);
    cfg.on_forest = nullptr;
    cfg.leaf.threads = 1;
    const auto b = apex::cluster_merge(inst, g, cfg);
    CHECK(a.order == b.order);
    const auto bin = apex::decompose_and_patch(inst, g, cfg);
    CHECK(a.cost <= bin.cost * 1.01);
    MESSAGE("kway 20000: leaves=" << st.leaf.leaves << " rounds=" << st.levels << " cost=" << a.cost << " binary=" << bin.cost);
}

TEST_CASE("T_polish_segments") {
    const auto inst = random_instance(30000, 5, true);
    const auto g = apex::build_candidate_graph(inst);
    apex::DecompConfig dc;
    dc.leaf.ils_iterations_per_node = 1;
    dc.leaf.exact_limit = 0;
    dc.leaf.threads = 4;
    const auto start = apex::construct_tour(inst, g, dc);
    apex::PolishConfig pc;
    pc.segment_size = 5000;
    pc.rounds = 2;
    pc.iterations_per_node = 1;
    pc.threads = 2;
    auto a = start;
    const auto sa = apex::polish_tour(a, inst, g, pc);
    pc.threads = 5;
    auto b = start;
    const auto sb = apex::polish_tour(b, inst, g, pc);
    REQUIRE(apex::is_valid_tour(a, inst.n));
    CHECK(a.cost == apex::tour_cost(inst, a));
    CHECK(sa.parallel);
    CHECK(sa.segments == 6);
    CHECK(sa.rounds == 2);
    CHECK(a.cost <= start.cost);
    CHECK(a.order == b.order);  // deterministic for any thread count
    CHECK(sb.final_cost == sa.final_cost);
    MESSAGE("polish 30000: construct=" << start.cost << " polished=" << a.cost << " iterations=" << sa.iterations);
}

// GUI hook: every ILS worker reports the nodes it kicks; observing never changes the tour.
TEST_CASE("T_polish_on_work_observes") {
    const auto inst = random_instance(3000, 7, true);
    const auto g = apex::build_candidate_graph(inst);
    apex::Tour start;
    start.order.resize(inst.n);
    for (uint32_t k = 0; k < inst.n; ++k) start.order[k] = k;
    apex::PolishConfig pc;
    pc.threads = 3;
    pc.iterations_per_node = 1;
    auto a = start;
    apex::polish_tour(a, inst, g, pc);
    std::mutex mu;
    std::vector<uint64_t> calls(pc.threads, 0);
    bool in_range = true;
    pc.on_work = [&](uint32_t w, uint32_t v) {
        std::lock_guard<std::mutex> lk(mu);
        if (w >= calls.size() || v >= inst.n) { in_range = false; return; }
        ++calls[w];
    };
    auto b = start;
    apex::polish_tour(b, inst, g, pc);
    CHECK(in_range);
    for (uint64_t c : calls) CHECK(c > 0);
    CHECK(a.order == b.order);
}

TEST_CASE("T_add_tour_edges") {
    const auto inst = random_instance(2000, 6, false);
    auto g = apex::build_candidate_graph(inst);
    apex::Tour t;
    for (uint32_t k = 0; k < inst.n; ++k) t.order.push_back(k);
    const size_t before = g.edges.size();
    const uint32_t added = apex::add_tour_edges(g, inst, t);
    CHECK(g.edges.size() == before + added);
    CHECK(added > 0);
    CHECK(apex::add_tour_edges(g, inst, t) == 0);
    for (uint32_t k = 0; k < inst.n; ++k) {
        const uint32_t u = t.order[k], v = t.order[(k + 1) % inst.n];
        bool found = false;
        for (uint32_t p = g.adj_start[u]; p < g.adj_start[u + 1]; ++p) found = found || g.other(g.adj[p], u) == v;
        REQUIRE(found);
    }
}
