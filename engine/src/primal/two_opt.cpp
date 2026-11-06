#include "primal/two_opt.hpp"

#include "primal/local_search.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <random>

namespace apex {

Tour hilbert_tour(const Instance& inst, const std::vector<uint32_t>& hilbert_order) {
    Tour t;
    t.order = hilbert_order;
    t.cost = tour_cost(inst, t);
    return t;
}

TwoOptStats two_opt_oropt(Tour& tour, const Instance& inst, const CandidateGraph& g, const TwoOptConfig& cfg) {
    TwoOptStats st;
    const auto n = static_cast<uint32_t>(tour.order.size());
    if (n < 5) return st;

    uint32_t max_deg = 0;
    for (uint32_t i = 0; i < g.n; ++i) max_deg = std::max(max_deg, g.adj_start[i + 1] - g.adj_start[i]);
    const uint32_t k_max = cfg.max_neighbors == 0 ? max_deg : std::min(cfg.max_neighbors, max_deg);
    uint32_t k = std::max(1u, std::min(cfg.initial_neighbors, k_max));

    std::vector<uint32_t> start(tour.order);
    // Own Fisher-Yates: std::shuffle's algorithm differs between standard libraries,
    // mt19937_64's output does not, so the order is identical on every platform.
    std::mt19937_64 rng(cfg.seed);
    for (uint32_t i = n - 1; i > 0; --i) std::swap(start[i], start[rng() % (i + 1)]);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(cfg.time_limit_s));
    detail::LocalSearch ls(tour, inst, g);
    for (;;) {
        const uint64_t moves = ls.round(k, cfg.or_opt, start, deadline, cfg.time_limit_s > 0, st);
        ++st.rounds;
        st.final_neighbors = k;
        if (st.time_limit_hit || k >= k_max || (st.rounds > 1 && moves == 0)) break;
        k = std::min(k_max, k + std::max(1u, k / 2));
    }
    return st;
}

}  // namespace apex
