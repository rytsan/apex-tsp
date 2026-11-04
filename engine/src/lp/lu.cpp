#include "lp/lu.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace apex {
namespace {

struct RowEntry {
    uint32_t col;
    double val;
};

void erase_value(std::vector<uint32_t>& v, uint32_t x) {
    const auto it = std::find(v.begin(), v.end(), x);
    *it = v.back();
    v.pop_back();
}

}  // namespace

bool LuFactorization::factorize(uint32_t m, const std::vector<SparseCol>& cols, double threshold) {
    if (cols.size() != m) throw std::invalid_argument("LuFactorization: need m columns");
    m_ = m;
    steps_.clear();
    l_idx_.clear(); l_val_.clear(); u_idx_.clear(); u_val_.clear();
    singular_pos_.clear();
    unpivoted_rows_.clear();
    min_ratio_ = 1.0;
    b_nnz_ = 0;

    // Active submatrix: values row-wise, patterns column-wise.
    std::vector<std::vector<RowEntry>> rows(m);
    std::vector<std::vector<uint32_t>> colpat(m);
    for (uint32_t j = 0; j < m; ++j)
        for (const auto& e : cols[j]) {
            if (e.index >= m) throw std::out_of_range("LuFactorization: row index out of range");
            if (e.value == 0.0) continue;
            rows[e.index].push_back({j, e.value});
            colpat[j].push_back(e.index);
            ++b_nnz_;
        }
    // Merge duplicate (row, col) entries.
    for (uint32_t i = 0; i < m; ++i) {
        auto& r = rows[i];
        std::sort(r.begin(), r.end(), [](const RowEntry& a, const RowEntry& b) { return a.col < b.col; });
        size_t w = 0;
        for (size_t k = 0; k < r.size();) {
            RowEntry acc = r[k++];
            while (k < r.size() && r[k].col == acc.col) acc.val += r[k++].val;
            r[w++] = acc;
        }
        r.resize(w);
    }
    for (uint32_t j = 0; j < m; ++j) {
        auto& c = colpat[j];
        std::sort(c.begin(), c.end());
        c.erase(std::unique(c.begin(), c.end()), c.end());
    }

    std::set<std::pair<uint32_t, uint32_t>> col_q, row_q;  // (count, index)
    for (uint32_t j = 0; j < m; ++j) col_q.insert({static_cast<uint32_t>(colpat[j].size()), j});
    for (uint32_t i = 0; i < m; ++i) row_q.insert({static_cast<uint32_t>(rows[i].size()), i});
    std::vector<uint8_t> row_done(m, 0), col_done(m, 0);

    auto value_at = [&](uint32_t i, uint32_t j) {
        for (const auto& e : rows[i])
            if (e.col == j) return e.val;
        return 0.0;
    };
    auto col_max = [&](uint32_t j) {
        double mx = 0;
        for (uint32_t i : colpat[j]) mx = std::max(mx, std::abs(value_at(i, j)));
        return mx;
    };
    auto set_col_count = [&](uint32_t j, size_t before) {
        col_q.erase({static_cast<uint32_t>(before), j});
        col_q.insert({static_cast<uint32_t>(colpat[j].size()), j});
    };
    auto set_row_count = [&](uint32_t i, size_t before) {
        row_q.erase({static_cast<uint32_t>(before), i});
        row_q.insert({static_cast<uint32_t>(rows[i].size()), i});
    };

    std::vector<int32_t> slot(m, -1);  // scatter map col -> index inside the row being updated
    constexpr int kSearch = 4;         // limited Markowitz search (Zlatev-style)
    constexpr double kAbsPivotTol = 1e-12;  // entries this small are treated as zero

    for (uint32_t k = 0; k < m; ++k) {
        // Structurally empty columns are singular: drop them.
        while (!col_q.empty() && col_q.begin()->first == 0) {
            const uint32_t j = col_q.begin()->second;
            col_q.erase(col_q.begin());
            col_done[j] = 1;
            singular_pos_.push_back(j);
        }
        if (col_q.empty()) break;

        // Pivot search over the sparsest columns and rows.
        uint32_t best_r = m, best_c = m;
        double best_cost = std::numeric_limits<double>::infinity(), best_ratio = 0;
        auto consider = [&](uint32_t i, uint32_t j, double v, double cmax) {
            if (std::abs(v) <= kAbsPivotTol || std::abs(v) < threshold * cmax) return;
            const double cost = double(rows[i].size() - 1) * double(colpat[j].size() - 1);
            const double ratio = std::abs(v) / cmax;
            if (cost < best_cost || (cost == best_cost && (ratio > best_ratio ||
                                     (ratio == best_ratio && std::make_pair(j, i) < std::make_pair(best_c, best_r))))) {
                best_cost = cost; best_ratio = ratio; best_r = i; best_c = j;
            }
        };
        int seen = 0;
        for (auto it = col_q.begin(); it != col_q.end() && (seen < kSearch || best_r == m); ++it, ++seen) {
            const uint32_t j = it->second;
            const double cmax = col_max(j);
            for (uint32_t i : colpat[j]) consider(i, j, value_at(i, j), cmax);
        }
        seen = 0;
        for (auto it = row_q.begin(); it != row_q.end() && seen < kSearch; ++it, ++seen) {
            const uint32_t i = it->second;
            for (const auto& e : rows[i]) consider(i, e.col, e.val, col_max(e.col));
        }
        if (best_r == m) {  // every remaining column is numerically zero
            for (const auto& [cnt, j] : col_q) { (void)cnt; col_done[j] = 1; singular_pos_.push_back(j); }
            col_q.clear();
            break;
        }
        const uint32_t r = best_r, c = best_c;
        const double p = value_at(r, c);
        min_ratio_ = std::min(min_ratio_, best_ratio);

        Step st{r, c, p, static_cast<uint32_t>(l_idx_.size()), 0, static_cast<uint32_t>(u_idx_.size()), 0};
        // U row = pivot row (without the pivot itself); remove row r from active column patterns.
        for (const auto& e : rows[r]) {
            if (e.col != c) { u_idx_.push_back(e.col); u_val_.push_back(e.val); }
            const size_t before = colpat[e.col].size();
            erase_value(colpat[e.col], r);
            if (e.col != c) set_col_count(e.col, before);
        }
        st.u_end = static_cast<uint32_t>(u_idx_.size());
        col_q.erase({static_cast<uint32_t>(colpat[c].size() + 1), c});
        row_q.erase({static_cast<uint32_t>(rows[r].size()), r});

        // Eliminate column c from the other active rows.
        const std::vector<uint32_t> targets = colpat[c];
        for (uint32_t i : targets) {
            auto& ri = rows[i];
            const size_t row_before = ri.size();
            for (size_t q = 0; q < ri.size(); ++q) slot[ri[q].col] = static_cast<int32_t>(q);
            const double l = ri[slot[c]].val / p;
            l_idx_.push_back(i);
            l_val_.push_back(l);
            for (const auto& e : rows[r]) {
                if (e.col == c) continue;
                if (slot[e.col] >= 0) {
                    ri[slot[e.col]].val -= l * e.val;
                } else {  // fill-in
                    slot[e.col] = static_cast<int32_t>(ri.size());
                    ri.push_back({e.col, -l * e.val});
                    const size_t before = colpat[e.col].size();
                    colpat[e.col].push_back(i);
                    set_col_count(e.col, before);
                }
            }
            // Remove the eliminated entry (i, c).
            const size_t q = static_cast<size_t>(slot[c]);
            for (const auto& e : ri) slot[e.col] = -1;
            ri[q] = ri.back();
            ri.pop_back();
            set_row_count(i, row_before);
        }
        colpat[c].clear();
        rows[r].clear();
        row_done[r] = 1;
        col_done[c] = 1;
        st.l_end = static_cast<uint32_t>(l_idx_.size());
        steps_.push_back(st);
    }
    l_nnz_ = l_idx_.size();
    u_nnz_ = u_idx_.size() + steps_.size();
    if (steps_.size() == m) return true;
    for (uint32_t j = 0; j < m; ++j)
        if (!col_done[j]) singular_pos_.push_back(j);
    for (uint32_t i = 0; i < m; ++i)
        if (!row_done[i]) unpivoted_rows_.push_back(i);
    std::sort(singular_pos_.begin(), singular_pos_.end());
    return false;
}

void LuFactorization::ftran(std::vector<double>& a) const {
    // Apply the elimination (L) to the row-indexed right-hand side.
    for (const auto& s : steps_) {
        const double ar = a[s.row];
        if (ar == 0.0) continue;
        for (uint32_t q = s.l_begin; q < s.l_end; ++q) a[l_idx_[q]] -= l_val_[q] * ar;
    }
    // Back substitution on U; result indexed by basis position.
    work_.assign(m_, 0.0);
    for (size_t k = steps_.size(); k-- > 0;) {
        const auto& s = steps_[k];
        double v = a[s.row];
        for (uint32_t q = s.u_begin; q < s.u_end; ++q) v -= u_val_[q] * work_[u_idx_[q]];
        work_[s.col] = v / s.pivot;
    }
    a.swap(work_);
}

void LuFactorization::btran(std::vector<double>& d) const {
    // U^T z = d (forward over steps); z indexed by row.
    work_.assign(m_, 0.0);
    for (const auto& s : steps_) {
        const double z = d[s.col] / s.pivot;
        work_[s.row] = z;
        if (z == 0.0) continue;
        for (uint32_t q = s.u_begin; q < s.u_end; ++q) d[u_idx_[q]] -= z * u_val_[q];
    }
    // y = E^T z: apply transposed eliminations in reverse order.
    for (size_t k = steps_.size(); k-- > 0;) {
        const auto& s = steps_[k];
        double acc = 0;
        for (uint32_t q = s.l_begin; q < s.l_end; ++q) acc += l_val_[q] * work_[l_idx_[q]];
        work_[s.row] -= acc;
    }
    d.swap(work_);
}

}  // namespace apex
