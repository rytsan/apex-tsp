#include "primal/two_opt.hpp"

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

namespace {

class LocalSearch {
public:
    LocalSearch(Tour& t, const Instance& inst, const CandidateGraph& g)
        : t_(t), in_(inst), g_(g), n_(static_cast<uint32_t>(t.order.size())), pos_(n_), queued_(n_, 0) {
        for (uint32_t k = 0; k < n_; ++k) pos_[t_.order[k]] = k;
    }

    uint32_t succ(uint32_t a) const { return t_.order[pos_[a] + 1 == n_ ? 0 : pos_[a] + 1]; }
    uint32_t pred(uint32_t a) const { return t_.order[pos_[a] == 0 ? n_ - 1 : pos_[a] - 1]; }
    int64_t d(uint32_t a, uint32_t b) const { return dist(in_, a, b); }

    void push(uint32_t a) {
        if (!queued_[a]) { queued_[a] = 1; queue_.push_back(a); }
    }

    // Returns number of improving moves; stops early when the deadline passes.
    uint64_t round(uint32_t k_nb, bool or_opt, const std::vector<uint32_t>& start_order,
                   std::chrono::steady_clock::time_point deadline, bool has_deadline, TwoOptStats& st) {
        k_ = k_nb;
        for (uint32_t a : start_order) push(a);
        uint64_t moves = 0, iter = 0;
        while (!queue_.empty()) {
            if (has_deadline && (++iter & 255) == 0 && std::chrono::steady_clock::now() > deadline) {
                st.time_limit_hit = true;
                break;
            }
            const uint32_t a = queue_.front();
            queue_.pop_front();
            queued_[a] = 0;
            if (try_2opt(a)) { ++st.moves_2opt; ++moves; push(a); continue; }
            if (or_opt && try_oropt(a)) { ++st.moves_oropt; ++moves; push(a); continue; }
        }
        for (uint32_t a : queue_) queued_[a] = 0;
        queue_.clear();
        return moves;
    }

private:
    // Neighbours of i: first k_ edges of its cost-sorted adjacency.
    uint32_t nb_end(uint32_t i) const { return std::min(g_.adj_start[i] + k_, g_.adj_start[i + 1]); }

    // Reverse the forward path a..b (or its complement, whichever is shorter).
    void reverse(uint32_t a, uint32_t b) {
        uint32_t i = pos_[a], j = pos_[b];
        uint32_t len = (j + n_ - i) % n_ + 1;
        if (2 * len > n_) {
            i = j + 1 == n_ ? 0 : j + 1;
            j = pos_[a] == 0 ? n_ - 1 : pos_[a] - 1;
            len = n_ - len;
        }
        for (uint32_t s = 0; s < len / 2; ++s) {
            const uint32_t u = t_.order[i], v = t_.order[j];
            t_.order[i] = v; pos_[v] = i;
            t_.order[j] = u; pos_[u] = j;
            i = i + 1 == n_ ? 0 : i + 1;
            j = j == 0 ? n_ - 1 : j - 1;
        }
    }

    bool try_2opt(uint32_t t1) {
        for (int dir = 0; dir < 2; ++dir) {
            const uint32_t t2 = dir == 0 ? succ(t1) : pred(t1);
            const int64_t d12 = d(t1, t2);
            for (uint32_t p = g_.adj_start[t2]; p < nb_end(t2); ++p) {
                const uint32_t e = g_.adj[p];
                const uint32_t t3 = g_.other(e, t2);
                const int64_t d23 = g_.edges[e].cost;
                if (d12 - d23 <= 0) break;  // sorted neighbours: no further positive partial gain
                if (t3 == t1) continue;
                const uint32_t t4 = dir == 0 ? pred(t3) : succ(t3);
                if (t4 == t2) continue;
                const int64_t gain = d12 + d(t3, t4) - d23 - d(t1, t4);
                if (gain > 0) {
                    // dir 0: t1 t2 .. t4 t3 -> reverse t2..t4 ; dir 1: t3 t4 .. t2 t1 -> reverse t4..t2
                    if (dir == 0) reverse(t2, t4); else reverse(t4, t2);
                    t_.cost -= gain;
                    push(t2); push(t3); push(t4);
                    return true;
                }
            }
        }
        return false;
    }

    bool in_seg(uint32_t v, uint32_t ps, uint32_t len) const { return (pos_[v] + n_ - ps) % n_ < len; }

    // Move segment [s1..s2] (forward, length len, starting at position ps) between a and b = succ(a).
    void move_segment(uint32_t ps, uint32_t len, uint32_t a, uint32_t b, bool reversed) {
        uint32_t seg[3];
        for (uint32_t k = 0; k < len; ++k) seg[k] = t_.order[(ps + k) % n_];
        if (reversed) std::reverse(seg, seg + len);
        const uint32_t nx_pos = (ps + len) % n_;
        const uint32_t r1 = (pos_[a] + n_ - nx_pos) % n_ + 1;  // nx..a
        const uint32_t r2 = n_ - len - r1;                     // b..p
        auto put = [&](uint32_t at, uint32_t v) { t_.order[at % n_] = v; pos_[v] = at % n_; };
        if (r1 <= r2) {  // shift nx..a back by len, segment goes after a
            for (uint32_t k = 0; k < r1; ++k) put(ps + k, t_.order[(ps + len + k) % n_]);
            for (uint32_t k = 0; k < len; ++k) put(ps + r1 + k, seg[k]);
        } else {  // shift b..p forward by len, segment goes before b
            const uint32_t qb = pos_[b];
            for (uint32_t k = r2; k-- > 0;) put(qb + k + len, t_.order[(qb + k) % n_]);
            for (uint32_t k = 0; k < len; ++k) put(qb + k, seg[k]);
        }
    }

    bool try_oropt(uint32_t t1) {
        if (n_ < 8) return false;
        for (uint32_t len = 1; len <= 3; ++len) {
            for (int side = 0; side < 2; ++side) {  // t1 is the first or the last node of the segment
                if (len == 1 && side == 1) break;
                const uint32_t ps = side == 0 ? pos_[t1] : (pos_[t1] + n_ - (len - 1)) % n_;
                const uint32_t s1 = t_.order[ps], s2 = t_.order[(ps + len - 1) % n_];
                const uint32_t p = pred(s1), nx = succ(s2);
                const int64_t g1 = d(p, s1) + d(s2, nx) - d(p, nx);
                if (g1 <= 0) continue;
                for (uint32_t end = 0; end < 2; ++end) {
                    const uint32_t s = end == 0 ? s1 : s2;
                    for (uint32_t q = g_.adj_start[s]; q < nb_end(s); ++q) {
                        const uint32_t e = g_.adj[q];
                        const uint32_t c = g_.other(e, s);
                        if (g_.edges[e].cost >= g1) break;
                        if (in_seg(c, ps, len)) continue;
                        for (int side_c = 0; side_c < 2; ++side_c) {
                            const uint32_t a = side_c == 0 ? pred(c) : c;
                            const uint32_t b = side_c == 0 ? c : succ(c);
                            if (in_seg(a, ps, len) || in_seg(b, ps, len)) continue;
                            const int64_t dab = d(a, b);
                            const int64_t fwd = g1 + dab - d(a, s1) - d(s2, b);  // a s1..s2 b
                            const int64_t rev = g1 + dab - d(a, s2) - d(s1, b);  // a s2..s1 b
                            if (fwd > 0 || rev > 0) {
                                const bool use_rev = rev > fwd;
                                move_segment(ps, len, a, b, use_rev);
                                t_.cost -= use_rev ? rev : fwd;
                                push(p); push(nx); push(a); push(b); push(s1); push(s2);
                                return true;
                            }
                        }
                    }
                }
            }
        }
        return false;
    }

    Tour& t_;
    const Instance& in_;
    const CandidateGraph& g_;
    uint32_t n_;
    uint32_t k_ = 10;
    std::vector<uint32_t> pos_;
    std::vector<uint8_t> queued_;
    std::deque<uint32_t> queue_;
};

}  // namespace

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
    LocalSearch ls(tour, inst, g);
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
