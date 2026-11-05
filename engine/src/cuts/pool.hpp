#pragma once

#include <cstdint>
#include <istream>
#include <mutex>
#include <ostream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cuts/blossom.hpp"
#include "cuts/support.hpp"
#include "graph/candidate_graph.hpp"
#include "lp/csc.hpp"

namespace apex {

enum class CutKind : uint8_t { Sec, Blossom };

struct PoolCut {
    CutKind kind;
    std::vector<uint32_t> nodes;                          // SEC: S; blossom: handle (sorted)
    std::vector<std::pair<uint32_t, uint32_t>> teeth;     // blossom teeth endpoints (u < v), sorted
    uint64_t hash = 0;
    uint64_t id = 0;              // insertion sequence (deterministic)
    int32_t lp_row = -1;          // row index while in the LP, -1 in the pool
    uint32_t inactive_rounds = 0; // consecutive re-solves with zero dual
};

struct PoolStats {
    uint64_t inserted = 0, duplicates = 0, aged_out = 0, reactivated = 0;
    size_t size = 0, in_lp = 0;
};

// Canonical cut storage with 64-bit hash dedup and aging.
// Thread-safe staging: separators may call stage_*() concurrently; commit() sorts the staged
// cuts by their canonical representation before assigning ids, so the resulting order is
// identical regardless of thread interleaving.
class CutPool {
public:
    explicit CutPool(uint32_t age_limit = 10) : age_limit_(age_limit) {}
    // Copy (tree workers start from a snapshot); staging must be empty, the mutex is not copied.
    CutPool(const CutPool& o)
        : age_limit_(o.age_limit_), cuts_(o.cuts_), by_hash_(o.by_hash_), next_id_(o.next_id_), stats_(o.stats_) {}
    CutPool& operator=(const CutPool&) = delete;

    // Index of an identical cut, inserting a copy (lp_row = -1) if absent (worker merge).
    size_t add_or_find(const PoolCut& c);
    // Checkpoint v2: exact state (cuts incl. lp_row / aging, ids, stats).
    void save(std::ostream& o) const;
    void load(std::istream& in);

    void stage_sec(const SecCut& c);          // thread-safe
    void stage_blossom(const BlossomCut& c);  // thread-safe
    // Moves staged cuts into the pool (dedup); returns indices of newly inserted cuts.
    std::vector<size_t> commit();

    size_t size() const { return cuts_.size(); }
    PoolCut& at(size_t i) { return cuts_[i]; }
    const PoolCut& at(size_t i) const { return cuts_[i]; }

    // LP row of a cut over the LP columns (endpoints lp_edges[j]):
    // SEC x(delta(S)) >= 2; blossom x(delta(H) \ T) - x(T) >= 1 - |T|.
    Row make_row(const PoolCut& c, const std::vector<Edge>& lp_edges) const;
    // Violation of a cut for LP solution x (> 0 means violated).
    double violation(const PoolCut& c, const std::vector<Edge>& lp_edges, const std::vector<double>& x) const;

    // Aging after a re-solve: duals[row] of in-LP cuts; cuts with |dual| <= tol for age_limit
    // consecutive re-solves leave the LP. Returns pool indices to remove from the LP
    // (caller deletes those rows and calls set_lp_row(idx, -1)).
    std::vector<size_t> age(const std::vector<double>& duals, double tol = 1e-9);
    void set_lp_row(size_t idx, int32_t row) { cuts_[idx].lp_row = row; if (row >= 0) cuts_[idx].inactive_rounds = 0; }

    // Pool cuts currently outside the LP that are violated by x (> tol), most violated first.
    std::vector<size_t> violated_outside(const std::vector<Edge>& lp_edges, const std::vector<double>& x,
                                         double tol = 1e-5) const;

    PoolStats stats() const;
    void note_reactivated(size_t k) { stats_.reactivated += k; }

private:
    bool insert(PoolCut&& c);
    static uint64_t hash_of(const PoolCut& c);

    uint32_t age_limit_;
    std::vector<PoolCut> cuts_;
    std::unordered_map<uint64_t, std::vector<size_t>> by_hash_;
    std::vector<PoolCut> staged_;
    mutable std::mutex mu_;
    uint64_t next_id_ = 0;
    PoolStats stats_;
};

}  // namespace apex
