#include "decomp/leaf_solver.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <thread>

#include "bnc/tree.hpp"
#include "geo/hilbert.hpp"
#include "primal/lk_lite.hpp"
#include "primal/lp_tour.hpp"
#include "primal/two_opt.hpp"

namespace apex {

Instance sub_instance(const Instance& inst, const std::vector<uint32_t>& ids) {
    Instance s;
    s.name = inst.name + "_leaf";
    s.n = static_cast<uint32_t>(ids.size());
    s.metric = inst.metric;
    for (uint32_t v : ids) {
        s.xs.push_back(inst.xs[v]);
        s.ys.push_back(inst.ys[v]);
        s.original_ids.push_back(inst.original_ids.empty() ? v + 1 : inst.original_ids[v]);
    }
    return s;
}

namespace {

uint64_t mix(uint64_t x) {  // splitmix64 finalizer
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// heuristic: tour of one leaf in local ids (greedy + 2-opt + ILS, then mini B&C if small); *exact / *proven report the mini B&C path.
std::vector<uint32_t> solve_leaf(const Instance& sub, const LeafConfig& cfg, uint64_t seed, bool* exact, bool* proven) {
    if (sub.n <= 3) {
        std::vector<uint32_t> t(sub.n);
        for (uint32_t k = 0; k < sub.n; ++k) t[k] = k;
        return t;
    }
    auto g = build_candidate_graph(sub);
    // Greedy edge start (candidate graph by cost, fragments joined nearest-first) + 2-opt/Or-opt.
    Tour t = lp_guided_tour(sub, g, {});
    IlsConfig ic;
    ic.seed = seed;
    ic.max_iterations = cfg.ils_iterations_per_node * sub.n;
    iterated_local_search(t, sub, g, ic);
    if (sub.n <= cfg.exact_limit && sub.n >= 5) {
        BnCConfig bc;
        bc.max_nodes = cfg.exact_max_nodes;
        bc.seed = seed;
        bc.rih_iterations = 0;
        const auto r = run_bnc(sub, g, bc, t);
        *exact = true;
        *proven = r.status == BnCResult::Status::Optimal;
        if (r.ub < t.cost) t = r.best;
    }
    return t.order;
}

}  // namespace

std::vector<std::vector<uint32_t>> solve_leaves(const Instance& inst, const std::vector<KdNode>& tree,
                                                const LeafConfig& cfg, LeafStats* stats) {
    std::vector<size_t> leaves;
    for (size_t k = 0; k < tree.size(); ++k)
        if (tree[k].left < 0) leaves.push_back(k);
    std::vector<std::vector<uint32_t>> out(tree.size());
    std::vector<uint8_t> exact(leaves.size(), 0), proven(leaves.size(), 0);
    std::atomic<size_t> next{0};
    std::vector<std::exception_ptr> err(std::max(1u, cfg.threads));
    auto work = [&](uint32_t tid) {
        try {
        for (size_t i; (i = next.fetch_add(1)) < leaves.size();) {
            const auto& ids = tree[leaves[i]].points;
            bool ex = false, pr = false;
            if (cfg.on_cluster) cfg.on_cluster(static_cast<uint32_t>(leaves[i]), 1);
            auto local = solve_leaf(sub_instance(inst, ids), cfg, mix(cfg.seed ^ mix(i)), &ex, &pr);
            for (auto& v : local) v = ids[v];
            if (cfg.on_leaf) cfg.on_leaf(static_cast<uint32_t>(leaves[i]), local);
            out[leaves[i]] = std::move(local);
            exact[i] = ex;
            proven[i] = pr;
            if (cfg.on_cluster) cfg.on_cluster(static_cast<uint32_t>(leaves[i]), 0);
        }
        } catch (...) {  // e.g. std::bad_alloc under the memory cap: rethrown on the caller's thread
            err[tid] = std::current_exception();
            next.store(leaves.size());
        }
    };
    const uint32_t nt = std::max(1u, std::min<uint32_t>(cfg.threads, static_cast<uint32_t>(leaves.size())));
    std::vector<std::thread> pool;
    for (uint32_t t = 1; t < nt; ++t) pool.emplace_back(work, t);
    work(0);
    for (auto& th : pool) th.join();
    for (auto& e : err)
        if (e) std::rethrow_exception(e);
    if (stats) {
        *stats = LeafStats{};
        stats->leaves = static_cast<uint32_t>(leaves.size());
        stats->min_size = UINT32_MAX;
        for (size_t i = 0; i < leaves.size(); ++i) {
            const auto sz = static_cast<uint32_t>(tree[leaves[i]].points.size());
            stats->min_size = std::min(stats->min_size, sz);
            stats->max_size = std::max(stats->max_size, sz);
            stats->exact_leaves += exact[i];
            stats->exact_proven += proven[i];
        }
    }
    return out;
}

}  // namespace apex
