#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "core/instance.hpp"
#include "graph/candidate_graph.hpp"
#include "graph/tours.hpp"

namespace apex {

// Heuristic (UB only). LK-lite local search + iterated local search (M4.1).
struct IlsConfig {
    uint64_t seed = 1;
    uint32_t lk_depth = 5;              // 2-opt steps per LK chain (sequential up to 6-opt)
    uint64_t max_iterations = 0;        // 0 = until time limit / stop
    double time_limit_s = 0;            // 0 = none
    uint32_t max_segment = 50;          // double-bridge segment length cap
    uint32_t strong_kick_after = 1000;  // stalled iterations before a strong kick (3 double bridges)
    uint32_t publish_every = 64;        // on_best called at most once per this many iterations
};

struct IlsStats {
    uint64_t iterations = 0, improvements = 0, equal_accepted = 0, strong_kicks = 0, lk_moves = 0;
    int64_t start_cost = 0, best_cost = 0;
    double seconds = 0;
};

// LK-lite descent on `tour` (all candidate neighbours, don't-look bits, Or-opt), then ILS:
// segment-local double bridge, LK-lite repair from the kicked nodes, accept if better or equal,
// otherwise roll back to the best tour (undo log). After strong_kick_after stalled iterations
// the kick is three double bridges. Deterministic for a given seed and iteration count.
// on_best(tour) gets the best tour after improvements (throttled) and once at the end.
IlsStats iterated_local_search(Tour& tour, const Instance& inst, const CandidateGraph& g, const IlsConfig& cfg,
                               const std::atomic<bool>* stop = nullptr,
                               const std::function<void(const Tour&)>& on_best = nullptr);

// ILS in a background thread for the engine lifetime (owns a copy of the candidate graph: the
// B&C graph grows under pricing). Improvements are published under a mutex; consumers poll at
// node boundaries.
class BackgroundIls {
public:
    BackgroundIls(const Instance& inst, CandidateGraph g, Tour start, IlsConfig cfg);
    ~BackgroundIls();
    BackgroundIls(const BackgroundIls&) = delete;
    BackgroundIls& operator=(const BackgroundIls&) = delete;

    int64_t best_cost() const { return best_cost_.load(std::memory_order_acquire); }
    // Copies the published best tour into `out` iff its cost < ub.
    bool take_if_better(int64_t ub, Tour& out) const;
    void stop();                 // idempotent; joins the thread
    IlsStats stats() const;      // final after stop()

private:
    const Instance& inst_;
    CandidateGraph g_;
    IlsConfig cfg_;
    Tour work_;
    mutable std::mutex mu_;
    Tour best_;
    IlsStats stats_;
    std::atomic<int64_t> best_cost_;
    std::atomic<bool> stop_{false};
    std::thread th_;
};

}  // namespace apex
