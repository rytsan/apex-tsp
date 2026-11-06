#pragma once

#include <cstdint>
#include <vector>

#include "lp/lu.hpp"

namespace apex {

// Forrest-Tomlin basis update (M4.6) on top of a fresh LuFactorization: B = L R^-1 U with
// L the elimination of the factorization (never modified), R a product of row etas (one per
// update) and U upper triangular in a permuted step order, kept both row-wise and column-wise.
// Replacing basis position p: the spike U * (B^-1 a_q) becomes column p of U, step p moves to
// the end of the order, its old row is eliminated against the following rows (multipliers =
// one row eta) and its new diagonal is what remains in column p.
//
// FTRAN / BTRAN have a hypersparse mode: only steps reached from the nonzeros of the right-hand
// side are visited (heap on the step order, scatter on the column- / row-wise copies) instead
// of all m steps. `touched` counts visited steps + entries (instrumentation).
class FtFactor {
public:
    void init(const LuFactorization& lu);
    // save_spike: keep R L^-1 a (the FT spike) for the next update() (entering column).
    void ftran(std::vector<double>& a, bool hyper, bool save_spike = false) const;  // row-indexed in, position-indexed out
    void btran(std::vector<double>& d, bool hyper) const;  // position-indexed in, row-indexed out
    // false: the new diagonal is numerically zero (caller refactorizes).
    bool update(uint32_t pos, const std::vector<double>& aq);
    uint32_t updates() const { return updates_; }
    size_t growth_nnz() const { return growth_; }  // row-eta entries + spike entries since init
    size_t nnz() const;
    mutable uint64_t touched = 0;

private:
    struct UStep {
        uint32_t row = 0, pos = 0;
        double pivot = 1;
        std::vector<std::pair<uint32_t, double>> entries;  // (position, value), off-diagonal
    };
    struct RowEta {
        uint32_t row;
        uint32_t begin, end;  // (row_j, multiplier) in ridx_ / rval_
    };

    uint32_t m_ = 0;
    // L: elimination steps of the factorization (pivot row, multipliers on later rows).
    std::vector<uint32_t> lrow_, lbeg_, lend_, lidx_;
    std::vector<double> lval_;
    std::vector<int32_t> lstep_of_row_;
    std::vector<std::vector<std::pair<uint32_t, double>>> lt_;  // lt_[row i]: (step k, l) with i in L column k
    // U
    std::vector<UStep> us_;
    std::vector<uint32_t> order_, rank_, step_of_pos_, step_of_row_;
    std::vector<std::vector<std::pair<uint32_t, double>>> col_;  // col_[pos]: (step, value) off-diagonal
    // R
    std::vector<RowEta> r_;
    std::vector<uint32_t> ridx_;
    std::vector<double> rval_;
    uint32_t updates_ = 0;
    size_t growth_ = 0;
    mutable std::vector<double> work_, val_, spike_;
    mutable bool spike_valid_ = false;
    mutable std::vector<uint8_t> mark_;
    mutable std::vector<uint32_t> heap_;
};

}  // namespace apex
