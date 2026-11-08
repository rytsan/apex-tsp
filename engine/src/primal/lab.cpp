#include "primal/lab.hpp"

#include <algorithm>
#include <chrono>
#include <numeric>
#include <random>
#include <sstream>

#include "decomp/pipeline.hpp"
#include "geo/grid.hpp"
#include "geo/hilbert.hpp"
#include "primal/lk_lite.hpp"
#include "primal/lp_tour.hpp"
#include "primal/two_opt.hpp"

namespace apex {

const std::vector<std::string>& lab_step_names() {
    static const std::vector<std::string> names = {"nn", "greedy", "hilbert", "random", "decomp",
                                                   "two_opt", "two_opt_oropt", "lk", "ils"};
    return names;
}

Tour nearest_neighbor_tour(const Instance& inst, const CandidateGraph& g, uint32_t start) {
    const uint32_t n = inst.n;
    Tour t;
    if (n == 0) return t;
    std::vector<uint8_t> seen(n, 0);
    t.order.reserve(n);
    const SpatialGrid grid(inst);
    uint32_t cur = start % n;
    uint32_t left = n;  // ponytail: unvisited count only; the grid search skips visited nodes, worst case O(n) per step
    for (;;) {
        seen[cur] = 1;
        t.order.push_back(cur);
        if (--left == 0) break;
        uint32_t best = UINT32_MAX;
        int32_t bd = INT32_MAX;
        for (uint32_t k = g.adj_start[cur]; k < g.adj_start[cur + 1]; ++k) {
            const auto& e = g.edges[g.adj[k]];
            const uint32_t w = e.u == cur ? e.v : e.u;
            if (!seen[w] && (e.cost < bd || (e.cost == bd && w < best))) { bd = e.cost; best = w; }
        }
        for (double r = grid.cell_size(); best == UINT32_MAX; r *= 2) {  // all neighbours used
            grid.for_each_in_radius(cur, r, [&](uint32_t w) {
                if (seen[w]) return;
                const int32_t d = dist(inst, cur, w);
                if (d < bd || (d == bd && w < best)) { bd = d; best = w; }
            });
            if (r > 1e18) break;
        }
        if (best == UINT32_MAX)  // defensive: any unvisited node
            for (uint32_t w = 0; w < n && best == UINT32_MAX; ++w)
                if (!seen[w]) best = w;
        cur = best;
    }
    t.cost = tour_cost(inst, t);
    return t;
}

Tour random_tour(const Instance& inst, uint64_t seed) {
    Tour t;
    t.order.resize(inst.n);
    std::iota(t.order.begin(), t.order.end(), 0u);
    std::mt19937_64 rng(seed);
    std::shuffle(t.order.begin(), t.order.end(), rng);
    t.cost = tour_cost(inst, t);
    return t;
}

LabStep run_lab_step(const std::string& command, Tour& tour, const Instance& inst, const CandidateGraph& g, uint64_t seed,
                     uint32_t threads, const std::function<void(const Tour&)>& on_tour) {
    std::istringstream in(command);
    LabStep s;
    in >> s.name;
    std::string arg;
    in >> arg;
    s.before = tour.order.empty() ? 0 : tour.cost;
    const auto t0 = std::chrono::steady_clock::now();
    const bool needs_tour = s.name == "two_opt" || s.name == "two_opt_oropt" || s.name == "lk" || s.name == "ils";
    if (needs_tour && !is_valid_tour(tour, inst.n)) {
        s.error = "no tour yet: run a construction first";
        return s;
    }
    if (s.name == "nn") {
        uint32_t start = 0;
        if (!arg.empty()) start = static_cast<uint32_t>(std::stoul(arg));
        tour = nearest_neighbor_tour(inst, g, start);
    } else if (s.name == "greedy") {
        tour = lp_guided_tour(inst, g, {}, false);
    } else if (s.name == "hilbert") {
        tour = hilbert_tour(inst, hilbert_order(inst));
    } else if (s.name == "random") {
        tour = random_tour(inst, seed);
    } else if (s.name == "decomp") {
        DecompConfig dc;
        dc.leaf.threads = threads;
        dc.leaf.seed = seed;
        tour = construct_tour(inst, g, dc);
    } else if (s.name == "two_opt" || s.name == "two_opt_oropt") {
        TwoOptConfig c;
        c.or_opt = s.name == "two_opt_oropt";
        c.seed = seed;
        const auto st = two_opt_oropt(tour, inst, g, c);
        s.moves = st.moves_2opt + st.moves_oropt;
    } else if (s.name == "lk") {
        std::vector<uint32_t> all(inst.n);
        std::iota(all.begin(), all.end(), 0u);
        s.moves = lk_descent(tour, inst, g, all);
    } else if (s.name == "ils") {
        IlsConfig c;
        c.seed = seed;
        c.time_limit_s = arg.empty() ? 5.0 : std::stod(arg);
        if (!(c.time_limit_s > 0) || c.time_limit_s > 86400) {
            s.error = "ils seconds must be in (0, 86400]";
            return s;
        }
        const auto st = iterated_local_search(tour, inst, g, c, nullptr, on_tour);
        s.moves = st.improvements;
    } else {
        s.error = "unknown step '" + s.name + "'";
        return s;
    }
    s.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    tour.cost = tour_cost(inst, tour);  // recomputed with the metric, never trusted
    s.ok = is_valid_tour(tour, inst.n);
    if (!s.ok) s.error = "internal error: step returned an invalid tour";
    s.after = tour.cost;
    return s;
}

}  // namespace apex
