#include "decomp/pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <set>
#include <thread>

#include "primal/lk_lite.hpp"
#include "primal/lp_tour.hpp"

namespace apex {

namespace {

using Clock = std::chrono::steady_clock;
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

template <class F>
void parallel_for(size_t count, uint32_t threads, F&& work) {
    std::atomic<size_t> next{0};
    std::vector<std::exception_ptr> err(std::max(1u, threads));
    auto run = [&](uint32_t tid) {
        try {
            for (size_t i; (i = next.fetch_add(1)) < count;) work(i);
        } catch (...) {
            err[tid] = std::current_exception();
            next.store(count);
        }
    };
    const uint32_t nt = std::max(1u, std::min<uint32_t>(threads, static_cast<uint32_t>(count)));
    std::vector<std::thread> pool;
    for (uint32_t t = 1; t < nt; ++t) pool.emplace_back(run, t);
    run(0);
    for (auto& th : pool) th.join();
    for (auto& e : err)
        if (e) std::rethrow_exception(e);
}

}  // namespace

Tour construct_tour(const Instance& inst, const CandidateGraph& g, const DecompConfig& cfg, ConstructStats* stats) {
    const auto t0 = Clock::now();
    ConstructStats st;
    Tour t;
    if (inst.n > 2 * cfg.max_leaf) {
        st.decomposed = true;
        t = decompose_and_patch(inst, g, cfg, &st.decomp);
    } else {
        t = lp_guided_tour(inst, g, {});  // greedy edge + 2-opt / Or-opt
    }
    t.cost = tour_cost(inst, t);
    st.seconds = since(t0);
    if (stats) *stats = st;
    return t;
}

PolishStats polish_tour(Tour& tour, const Instance& inst, const CandidateGraph& g, const PolishConfig& cfg) {
    const auto t0 = Clock::now();
    PolishStats st;
    const uint32_t n = inst.n;
    tour.cost = tour_cost(inst, tour);
    st.start_cost = tour.cost;
    const bool timed = cfg.time_limit_s > 0;
    const uint32_t rounds = cfg.rounds ? cfg.rounds : (timed ? UINT32_MAX : 1);
    if (cfg.threads <= 1 || n < 2 * cfg.segment_size) {  // whole tour, one thread
        IlsConfig ic;
        ic.seed = cfg.seed;
        if (timed) ic.time_limit_s = cfg.time_limit_s;
        else ic.max_iterations = cfg.iterations_per_node * n * rounds;
        const auto is = iterated_local_search(tour, inst, g, ic);
        st.rounds = 1;
        st.segments = 1;
        st.iterations = is.iterations;
    } else {
        st.parallel = true;
        const uint32_t k = (n + cfg.segment_size - 1) / cfg.segment_size;
        const uint32_t len = n / k;
        const uint32_t waves = (k + cfg.threads - 1) / cfg.threads;
        // Round budget: a tenth of the total (at least 1 s) split over the waves of segments.
        const double round_s = timed ? std::max(1.0, cfg.time_limit_s / 10) : 0;
        std::vector<uint64_t> iters(k, 0);
        for (uint32_t r = 0; r < rounds; ++r) {
            const double left = cfg.time_limit_s - since(t0);
            if (timed && left <= 0.5) break;
            const uint32_t offset = static_cast<uint32_t>((uint64_t(r) * (len / 2)) % n);
            const double seg_s = timed ? std::min(round_s, left) / waves : 0;
            parallel_for(k, cfg.threads, [&](size_t s) {
                const uint32_t begin = offset + static_cast<uint32_t>(s) * len;
                const uint32_t m = s + 1 == k ? n - static_cast<uint32_t>(s) * len : len;
                std::vector<uint32_t> ids(m);
                for (uint32_t j = 0; j < m; ++j) ids[j] = tour.order[(begin + j) % n];
                const Instance sub = sub_instance(inst, ids);
                const auto sg = build_candidate_graph(sub);
                Tour p;
                p.order.resize(m);
                for (uint32_t j = 0; j < m; ++j) p.order[j] = j;
                IlsConfig ic;
                ic.seed = mix64(cfg.seed ^ mix64((uint64_t(r) << 32) | s));
                ic.fixed_u = m - 1;  // path with fixed endpoints: closing edge stays
                ic.fixed_v = 0;
                if (timed) ic.time_limit_s = seg_s;
                else ic.max_iterations = cfg.iterations_per_node * m;
                iters[s] += iterated_local_search(p, sub, sg, ic).iterations;
                // Rotate the cycle back into the path 0 .. m-1 and write it in place.
                uint32_t z = 0;
                while (p.order[z] != 0) ++z;
                const bool fwd = p.order[(z + m - 1) % m] == m - 1;
                for (uint32_t j = 0; j < m; ++j)
                    tour.order[(begin + j) % n] = ids[p.order[fwd ? (z + j) % m : (z + m - j) % m]];
            });
            ++st.rounds;
        }
        st.segments = k;
        for (uint64_t x : iters) st.iterations += x;
        tour.cost = tour_cost(inst, tour);
    }
    st.final_cost = tour.cost;
    st.seconds = since(t0);
    return st;
}

uint32_t add_tour_edges(CandidateGraph& g, const Instance& inst, const Tour& t) {
    std::set<std::pair<uint32_t, uint32_t>> have;
    for (const auto& e : g.edges) have.insert({e.u, e.v});
    uint32_t added = 0;
    const size_t n = t.order.size();
    for (size_t k = 0; k < n; ++k) {
        uint32_t u = t.order[k], v = t.order[(k + 1) % n];
        if (u > v) std::swap(u, v);
        if (!have.insert({u, v}).second) continue;
        g.edges.push_back({u, v, dist(inst, u, v), kActive, 0.0});
        ++added;
    }
    if (added) rebuild_adjacency(g);
    return added;
}

}  // namespace apex
