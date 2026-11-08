#pragma once

// Array-based local search shared by two_opt_oropt (Sprint 0) and the LK-lite / ILS engine (M4.1).
// Internal header: heuristic code (UB only).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <vector>

#include "core/instance.hpp"
#include "core/control.hpp"
#include "graph/candidate_graph.hpp"
#include "graph/tours.hpp"
#include "primal/two_opt.hpp"

namespace apex::detail {

class LocalSearch {
public:
    LocalSearch(Tour& t, const Instance& inst, const CandidateGraph& g)
        : t_(t), in_(inst), g_(g), n_(static_cast<uint32_t>(t.order.size())), pos_(n_), queued_(n_, 0) {
        for (uint32_t k = 0; k < n_; ++k) pos_[t_.order[k]] = k;
    }

    uint32_t succ(uint32_t a) const { return t_.order[pos_[a] + 1 == n_ ? 0 : pos_[a] + 1]; }
    uint32_t pred(uint32_t a) const { return t_.order[pos_[a] == 0 ? n_ - 1 : pos_[a] - 1]; }
    uint32_t position(uint32_t a) const { return pos_[a]; }
    int64_t d(uint32_t a, uint32_t b) const { return dist(in_, a, b); }

    void push(uint32_t a) {
        if (!queued_[a]) { queued_[a] = 1; queue_.push_back(a); }
    }

    // 0: 2-opt moves (Sprint 0 behaviour); >= 1: LK-lite chains of up to `depth` 2-opt steps.
    void set_lk_depth(uint32_t depth) { lk_depth_ = depth; }
    uint64_t lk_moves() const { return lk_moves_; }
    // One tour edge no move may remove (segment polish: the closing edge of a path with fixed
    // endpoints). Reversals keep it; 2-opt / LK / Or-opt / double bridge never drop it.
    void set_fixed_edge(uint32_t a, uint32_t b) { fu_ = a; fv_ = b; }

    // Undo log: while on, array writes are recorded and rollback() restores the tour exactly.
    // The log holds at most n writes; past that it overflows and rollback copies the mirror (the
    // tour at the last commit, kept in sync from the log). Memory stays O(n).
    void log_begin() {
        if (mirror_.empty()) mirror_ = t_.order;
        logging_ = true;
        overflow_ = false;
        log_.clear();
        cost_mark_ = t_.cost;
    }
    void log_commit() {
        if (overflow_) mirror_ = t_.order;
        else
            for (const auto& w : log_) mirror_[w.i] = w.v;
        logging_ = overflow_ = false;
        log_.clear();
    }
    void rollback() {
        if (overflow_) {
            t_.order = mirror_;
            for (uint32_t k = 0; k < n_; ++k) pos_[t_.order[k]] = k;
        } else {
            for (size_t k = log_.size(); k-- > 0;) {
                const auto& w = log_[k];
                pos_[w.v] = w.old_pos;
                t_.order[w.i] = w.old_val;
            }
        }
        t_.cost = cost_mark_;
        logging_ = overflow_ = false;
        log_.clear();
    }

    // Segment-local double bridge: a [B: l1 nodes][C: l2 nodes] e  ->  a C B e, B starting at
    // tour position p + 1. Needs l1 + l2 + 2 <= n. Queues the six endpoints.
    // Returns false (tour unchanged) if the kick would remove the fixed edge.
    bool double_bridge(uint32_t p, uint32_t l1, uint32_t l2) {
        auto at = [&](uint32_t k) { return t_.order[(p + k) % n_]; };
        const uint32_t a = at(0), b1 = at(1), bl = at(l1), c1 = at(l1 + 1), cl = at(l1 + l2), e = at(l1 + l2 + 1);
        if (fixed(a, b1) || fixed(bl, c1) || fixed(cl, e)) return false;
        t_.cost += d(a, c1) + d(cl, b1) + d(bl, e) - d(a, b1) - d(bl, c1) - d(cl, e);
        seg_.clear();
        for (uint32_t k = 0; k < l2; ++k) seg_.push_back(at(l1 + 1 + k));
        for (uint32_t k = 0; k < l1; ++k) seg_.push_back(at(1 + k));
        for (uint32_t k = 0; k < l1 + l2; ++k) put(p + 1 + k, seg_[k]);
        push(a); push(b1); push(bl); push(c1); push(cl); push(e);
        return true;
    }

    // Returns number of improving moves; stops early when the deadline passes.
    uint64_t round(uint32_t k_nb, bool or_opt, const std::vector<uint32_t>& start_order,
                   std::chrono::steady_clock::time_point deadline, bool has_deadline, TwoOptStats& st) {
        k_ = k_nb;
        for (uint32_t a : start_order) push(a);
        uint64_t moves = 0, iter = 0;
        while (!queue_.empty()) {
            if ((++iter & 255) == 0) {
                pause_point();
                if (cancel_requested() || (has_deadline && std::chrono::steady_clock::now() > deadline)) {
                    st.time_limit_hit = true;
                    break;
                }
            }
            const uint32_t a = queue_.front();
            queue_.pop_front();
            queued_[a] = 0;
            if (lk_depth_ ? try_lk(a) : try_2opt(a)) { ++st.moves_2opt; ++moves; push(a); continue; }
            if (or_opt && try_oropt(a)) { ++st.moves_oropt; ++moves; push(a); continue; }
        }
        for (uint32_t a : queue_) queued_[a] = 0;
        queue_.clear();
        return moves;
    }

private:
    struct Write {
        uint32_t i, old_val, v, old_pos;
    };

    bool fixed(uint32_t a, uint32_t b) const { return (a == fu_ && b == fv_) || (a == fv_ && b == fu_); }

    void put(uint32_t at, uint32_t v) {
        at %= n_;
        if (logging_ && !overflow_) {
            if (log_.size() >= n_) { overflow_ = true; log_.clear(); }
            else log_.push_back({at, t_.order[at], v, pos_[v]});
        }
        t_.order[at] = v;
        pos_[v] = at;
    }

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
            put(i, v);
            put(j, u);
            i = i + 1 == n_ ? 0 : i + 1;
            j = j == 0 ? n_ - 1 : j - 1;
        }
    }

    // 2-opt move removing tour edges {x, y} and {u, v}: with a -> b and c -> d the forward
    // orientation of the two edges, the tour becomes a c .. b d (adds {a, c} and {b, d}).
    void move2(uint32_t x, uint32_t y, uint32_t u, uint32_t v) {
        const uint32_t b = succ(x) == y ? y : x;
        const uint32_t c = succ(u) == v ? u : v;
        reverse(b, c);
    }

    bool try_2opt(uint32_t t1) {
        for (int dir = 0; dir < 2; ++dir) {
            const uint32_t t2 = dir == 0 ? succ(t1) : pred(t1);
            if (fixed(t1, t2)) continue;
            const int64_t d12 = d(t1, t2);
            for (uint32_t p = g_.adj_start[t2]; p < nb_end(t2); ++p) {
                const uint32_t e = g_.adj[p];
                const uint32_t t3 = g_.other(e, t2);
                const int64_t d23 = g_.edges[e].cost;
                if (d12 - d23 <= 0) break;  // sorted neighbours: no further positive partial gain
                if (t3 == t1) continue;
                const uint32_t t4 = dir == 0 ? pred(t3) : succ(t3);
                if (t4 == t2 || fixed(t3, t4)) continue;
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

    // LK-lite (tabu-free, depth-limited): t1 keeps a tour neighbour t2 as the free end. Each level
    // picks t3 near t2 with positive partial gain, t4 = the neighbour of t3 that closes a tour,
    // and applies the 2-opt move (drops {t1,t2},{t3,t4}, adds {t2,t3},{t1,t4}); t4 becomes the new
    // t2. The chain is accepted at the first level whose closed gain is positive, else undone.
    // Breadth 5 / 3 / 1 at levels 0 / 1 / deeper, candidates ordered by d(t3,t4) - d(t2,t3).
    bool try_lk(uint32_t t1) {
        for (int side = 0; side < 2; ++side) {
            const uint32_t t2 = side == 0 ? succ(t1) : pred(t1);
            if (fixed(t1, t2)) continue;
            chain_.clear();
            if (lk_step(t1, t2, d(t1, t2), 0)) {
                push(t1);
                for (uint32_t v : chain_) push(v);
                ++lk_moves_;
                return true;
            }
        }
        return false;
    }

    bool lk_step(uint32_t t1, uint32_t t2, int64_t g, uint32_t level) {
        static constexpr uint32_t kBreadth[3] = {5, 3, 1};
        const uint32_t breadth = kBreadth[std::min(level, 2u)];
        struct Cand {
            int64_t key, d23, d34;
            uint32_t t3, t4;
        };
        Cand c[5];
        uint32_t nc = 0;
        const bool fwd = succ(t1) == t2;
        for (uint32_t p = g_.adj_start[t2]; p < nb_end(t2); ++p) {
            const uint32_t e = g_.adj[p];
            const uint32_t t3 = g_.other(e, t2);
            const int64_t d23 = g_.edges[e].cost;
            if (g - d23 <= 0) break;  // sorted neighbours
            if (t3 == t1 || t3 == succ(t2) || t3 == pred(t2)) continue;
            const uint32_t t4 = fwd ? pred(t3) : succ(t3);
            if (fixed(t3, t4)) continue;
            const int64_t d34 = d(t3, t4);
            const Cand x{d34 - d23, d23, d34, t3, t4};
            uint32_t k = nc < breadth ? nc++ : breadth;  // insertion into the top-`breadth` list
            if (k == breadth && x.key <= c[breadth - 1].key) continue;
            if (k == breadth) k = breadth - 1;
            while (k > 0 && c[k - 1].key < x.key) { c[k] = c[k - 1]; --k; }
            c[k] = x;
        }
        for (uint32_t i = 0; i < nc; ++i) {
            const Cand x = c[i];
            move2(t1, t2, x.t3, x.t4);
            const int64_t g1 = g - x.d23 + x.d34;
            const int64_t closed = g1 - d(x.t4, t1);
            chain_.push_back(t2); chain_.push_back(x.t3); chain_.push_back(x.t4);
            if (closed > 0) {
                t_.cost -= closed;
                return true;
            }
            if (level + 1 < lk_depth_ && lk_step(t1, x.t4, g1, level + 1)) return true;
            chain_.resize(chain_.size() - 3);
            move2(t1, x.t4, t2, x.t3);  // undo: drops {t1,t4},{t2,t3}, restores {t1,t2},{t3,t4}
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
                if (fixed(p, s1) || fixed(s2, nx)) continue;
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
                            if (in_seg(a, ps, len) || in_seg(b, ps, len) || fixed(a, b)) continue;
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
    uint32_t lk_depth_ = 0;
    uint32_t fu_ = UINT32_MAX, fv_ = UINT32_MAX;
    uint64_t lk_moves_ = 0;
    std::vector<uint32_t> pos_;
    std::vector<uint8_t> queued_;
    std::deque<uint32_t> queue_;
    std::vector<uint32_t> chain_, seg_;
    bool logging_ = false, overflow_ = false;
    std::vector<Write> log_;
    std::vector<uint32_t> mirror_;
    int64_t cost_mark_ = 0;
};

}  // namespace apex::detail
