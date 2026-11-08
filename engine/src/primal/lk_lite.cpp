#include "primal/lk_lite.hpp"

#include <algorithm>
#include <chrono>
#include <random>

#include "primal/local_search.hpp"

namespace apex {

IlsStats iterated_local_search(Tour& tour, const Instance& inst, const CandidateGraph& g, const IlsConfig& cfg,
                               const std::atomic<bool>* stop, const std::function<void(const Tour&)>& on_best) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    IlsStats st;
    const auto n = static_cast<uint32_t>(tour.order.size());
    tour.cost = tour_cost(inst, tour);
    st.start_cost = st.best_cost = tour.cost;
    if (n < 8) {
        if (on_best) on_best(tour);
        return st;
    }
    uint32_t k_max = 0;
    for (uint32_t i = 0; i < g.n; ++i) k_max = std::max(k_max, g.adj_start[i + 1] - g.adj_start[i]);

    detail::LocalSearch ls(tour, inst, g);
    ls.set_lk_depth(cfg.lk_depth);
    ls.set_fixed_edge(cfg.fixed_u, cfg.fixed_v);
    TwoOptStats ts;
    const auto no_deadline = Clock::time_point{};
    ls.round(k_max, true, tour.order, no_deadline, false, ts);  // LK-lite descent from every node

    std::mt19937_64 rng(cfg.seed);
    const uint32_t seg = std::max(1u, std::min(cfg.max_segment, (n - 2) / 6));
    const std::vector<uint32_t> none;
    uint64_t stall = 0, since_publish = 0, last_improvement = 0;
    bool dirty = true;
    auto timed_out = [&] {
        return cfg.time_limit_s > 0 && std::chrono::duration<double>(Clock::now() - t0).count() > cfg.time_limit_s;
    };
    while (cfg.max_iterations == 0 || st.iterations < cfg.max_iterations) {
        if (stop && stop->load(std::memory_order_relaxed)) break;
        pause_point();
        if (cancel_requested()) break;
        if ((st.iterations & 15) == 0 && timed_out()) break;
        if (cfg.max_stall && st.iterations - last_improvement >= cfg.max_stall) break;
        ++st.iterations;
        const int64_t before = tour.cost;
        ls.log_begin();
        const bool strong = stall >= cfg.strong_kick_after;
        for (int k = 0; k < (strong ? 3 : 1); ++k) {
            uint32_t p = static_cast<uint32_t>(rng() % n);
            const uint32_t l1 = 1 + static_cast<uint32_t>(rng() % seg);
            const uint32_t l2 = 1 + static_cast<uint32_t>(rng() % seg);
            if (!cfg.focus.empty())  // kick window covers a focus node
                p = (ls.position(cfg.focus[p % cfg.focus.size()]) + n - static_cast<uint32_t>(rng() % (l1 + l2 + 1))) % n;
            ls.double_bridge(p, l1, l2);
        }
        if (strong) { ++st.strong_kicks; stall = 0; }
        ls.round(k_max, true, none, no_deadline, false, ts);
        if (tour.cost < before) {
            ls.log_commit();
            ++st.improvements;
            last_improvement = st.iterations;
            stall = 0;
            dirty = true;
        } else if (tour.cost == before) {
            ls.log_commit();
            ++st.equal_accepted;
            ++stall;
        } else {
            ls.rollback();
            ++stall;
        }
        if (dirty && on_best && ++since_publish >= cfg.publish_every) {
            on_best(tour);
            dirty = false;
            since_publish = 0;
        }
    }
    st.best_cost = tour.cost;
    st.lk_moves = ls.lk_moves();
    st.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    if (on_best) on_best(tour);
    return st;
}

uint64_t lk_descent(Tour& tour, const Instance& inst, const CandidateGraph& g, const std::vector<uint32_t>& start,
                    uint32_t lk_depth) {
    if (tour.order.size() < 8) return 0;
    uint32_t k_max = 0;
    for (uint32_t i = 0; i < g.n; ++i) k_max = std::max(k_max, g.adj_start[i + 1] - g.adj_start[i]);
    detail::LocalSearch ls(tour, inst, g);
    ls.set_lk_depth(lk_depth);
    TwoOptStats ts;
    return ls.round(k_max, true, start, std::chrono::steady_clock::time_point{}, false, ts);
}

BackgroundIls::BackgroundIls(const Instance& inst, CandidateGraph g, Tour start, IlsConfig cfg)
    : inst_(inst), g_(std::move(g)), cfg_(cfg), work_(std::move(start)), best_(work_) {
    best_.cost = tour_cost(inst_, best_);
    best_cost_.store(best_.cost, std::memory_order_release);
    th_ = std::thread([this] {
        const auto st = iterated_local_search(work_, inst_, g_, cfg_, &stop_, [this](const Tour& t) {
            if (t.cost >= best_cost_.load(std::memory_order_relaxed)) return;
            std::lock_guard<std::mutex> lk(mu_);
            best_ = t;
            best_cost_.store(t.cost, std::memory_order_release);
        });
        std::lock_guard<std::mutex> lk(mu_);
        stats_ = st;
    });
}

BackgroundIls::~BackgroundIls() { stop(); }

void BackgroundIls::stop() {
    stop_.store(true, std::memory_order_relaxed);
    if (th_.joinable()) th_.join();
}

bool BackgroundIls::take_if_better(int64_t ub, Tour& out) const {
    if (best_cost() >= ub) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (best_.cost >= ub) return false;
    out = best_;
    return true;
}

IlsStats BackgroundIls::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
}

}  // namespace apex
