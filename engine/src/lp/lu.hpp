#pragma once

#include <cstdint>
#include <vector>

#include "lp/csc.hpp"

namespace apex {

using SparseCol = std::vector<Entry>;  // (row, value)

// Sparse LU of a square basis B (m x m, column k = basis position k) by right-looking
// Gaussian elimination with Markowitz pivot search and threshold stability:
// a pivot a_rc is acceptable only if |a_rc| >= threshold * max_i |a_ic| (active column).
// ponytail: active submatrix as vector-of-vectors + std::set count queues; switch to
// linked-list buckets (Suhl & Suhl) if the nucleus of large bases becomes a bottleneck.
class LuFactorization {
public:
    // Returns false if B is (numerically) singular; see singular_positions()/unpivoted_rows().
    bool factorize(uint32_t m, const std::vector<SparseCol>& cols, double threshold = 0.01);

    // Solve B x = a in place: input indexed by row, output indexed by basis position.
    void ftran(std::vector<double>& a) const;
    // Solve B^T y = d in place: input indexed by basis position, output indexed by row.
    void btran(std::vector<double>& d) const;

    uint32_t dim() const { return m_; }
    size_t nnz_factors() const { return l_nnz_ + u_nnz_; }
    // nnz(L) + nnz(U) - nnz(B) (U counts its diagonal).
    long long fill_in() const { return static_cast<long long>(nnz_factors()) - static_cast<long long>(b_nnz_); }
    double density() const { return m_ == 0 ? 0.0 : double(nnz_factors()) / (double(m_) * double(m_)); }

    // On failure: basis positions that received no pivot, and rows that received none.
    const std::vector<uint32_t>& singular_positions() const { return singular_pos_; }
    const std::vector<uint32_t>& unpivoted_rows() const { return unpivoted_rows_; }

    // Diagnostics for T_lu_markowitz_threshold: min over pivots of |pivot| / column max.
    double min_pivot_ratio() const { return min_ratio_; }

private:
    struct Step {
        uint32_t row, col;   // pivot row (constraint row) and column (basis position)
        double pivot;
        uint32_t l_begin, l_end;  // L entries (row i, multiplier) in l_idx_/l_val_
        uint32_t u_begin, u_end;  // U row entries (col j != col, value) in u_idx_/u_val_
    };

    uint32_t m_ = 0;
    size_t b_nnz_ = 0, l_nnz_ = 0, u_nnz_ = 0;
    double min_ratio_ = 1.0;
    std::vector<Step> steps_;
    std::vector<uint32_t> l_idx_, u_idx_;
    std::vector<double> l_val_, u_val_;
    std::vector<uint32_t> singular_pos_, unpivoted_rows_;
    mutable std::vector<double> work_;
};

}  // namespace apex
