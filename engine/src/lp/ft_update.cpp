#include "lp/ft_update.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace apex {

namespace {

void erase_first(std::vector<std::pair<uint32_t, double>>& v, uint32_t key) {
    for (size_t k = 0; k < v.size(); ++k)
        if (v[k].first == key) {
            v[k] = v.back();
            v.pop_back();
            return;
        }
}

}  // namespace

void FtFactor::init(const LuFactorization& lu) {
    m_ = lu.m_;
    const size_t steps = lu.steps_.size();
    lrow_.resize(steps); lbeg_.resize(steps); lend_.resize(steps);
    lidx_ = lu.l_idx_;
    lval_ = lu.l_val_;
    lstep_of_row_.assign(m_, -1);
    lt_.assign(m_, {});
    us_.assign(steps, UStep{});
    order_.resize(steps); rank_.resize(steps);
    step_of_pos_.assign(m_, 0);
    step_of_row_.assign(m_, 0);
    col_.assign(m_, {});
    for (uint32_t k = 0; k < steps; ++k) {
        const auto& s = lu.steps_[k];
        lrow_[k] = s.row; lbeg_[k] = s.l_begin; lend_[k] = s.l_end;
        lstep_of_row_[s.row] = static_cast<int32_t>(k);
        for (uint32_t q = s.l_begin; q < s.l_end; ++q) lt_[lu.l_idx_[q]].push_back({k, lu.l_val_[q]});
        auto& u = us_[k];
        u.row = s.row; u.pos = s.col; u.pivot = s.pivot;
        for (uint32_t q = s.u_begin; q < s.u_end; ++q) {
            u.entries.push_back({lu.u_idx_[q], lu.u_val_[q]});
            col_[lu.u_idx_[q]].push_back({k, lu.u_val_[q]});
        }
        order_[k] = rank_[k] = k;
        step_of_pos_[s.col] = k;
        step_of_row_[s.row] = k;
    }
    r_.clear(); ridx_.clear(); rval_.clear();
    updates_ = 0;
    growth_ = 0;
    val_.assign(m_, 0.0);
    mark_.assign(m_, 0);
    spike_valid_ = false;
}

size_t FtFactor::nnz() const {
    size_t n = lidx_.size() + ridx_.size() + us_.size();
    for (const auto& u : us_) n += u.entries.size();
    return n;
}

// Hypersparse traversal: visit keys (step ranks or step ids) from the seeds in increasing
// (ascending = true) or decreasing order; `visit` may push more keys through `push`.
#define APEX_FT_HEAP(ascending)                                                                          \
    auto cmp = [](uint32_t a, uint32_t b) { return ascending ? a > b : a < b; };                        \
    auto push = [&](uint32_t key) {                                                                      \
        if (mark_[key]) return;                                                                          \
        mark_[key] = 1;                                                                                  \
        heap_.push_back(key);                                                                            \
        std::push_heap(heap_.begin(), heap_.end(), cmp);                                                 \
    };                                                                                                   \
    auto pop = [&]() {                                                                                   \
        std::pop_heap(heap_.begin(), heap_.end(), cmp);                                                  \
        const uint32_t key = heap_.back();                                                               \
        heap_.pop_back();                                                                                \
        mark_[key] = 0;                                                                                  \
        return key;                                                                                      \
    };

void FtFactor::ftran(std::vector<double>& a, bool hyper, bool save_spike) const {
    const uint32_t steps = static_cast<uint32_t>(us_.size());
    // L: steps in increasing id; a row's value is final once all earlier steps ran.
    auto lstep = [&](uint32_t k) {
        ++touched;
        const double ar = a[lrow_[k]];
        if (ar == 0.0) return;
        for (uint32_t q = lbeg_[k]; q < lend_[k]; ++q) {
            ++touched;
            a[lidx_[q]] -= lval_[q] * ar;
        }
    };
    if (hyper) {
        heap_.clear();
        APEX_FT_HEAP(true)
        for (uint32_t i = 0; i < m_; ++i)
            if (a[i] != 0.0 && lstep_of_row_[i] >= 0) push(static_cast<uint32_t>(lstep_of_row_[i]));
        while (!heap_.empty()) {
            const uint32_t k = pop();
            const double ar = a[lrow_[k]];
            lstep(k);
            if (ar == 0.0) continue;
            for (uint32_t q = lbeg_[k]; q < lend_[k]; ++q)
                if (lstep_of_row_[lidx_[q]] >= 0) push(static_cast<uint32_t>(lstep_of_row_[lidx_[q]]));
        }
    } else {
        for (uint32_t k = 0; k < steps; ++k) lstep(k);
    }
    // R: row etas, oldest first.
    for (const auto& e : r_) {
        double acc = a[e.row];
        for (uint32_t q = e.begin; q < e.end; ++q) {
            ++touched;
            acc -= rval_[q] * a[ridx_[q]];
        }
        a[e.row] = acc;
    }
    if (save_spike) {
        spike_ = a;
        spike_valid_ = true;
    }
    // U: decreasing rank, column-wise scatter.
    work_.assign(m_, 0.0);
    auto ustep = [&](uint32_t k) {
        ++touched;
        const auto& u = us_[k];
        const double v = a[u.row];
        if (v == 0.0) return false;
        const double x = v / u.pivot;
        work_[u.pos] = x;
        for (const auto& [j, val] : col_[u.pos]) {
            ++touched;
            a[us_[j].row] -= val * x;
        }
        return true;
    };
    if (hyper) {
        heap_.clear();
        APEX_FT_HEAP(false)
        for (uint32_t i = 0; i < m_; ++i)
            if (a[i] != 0.0) push(rank_[step_of_row_[i]]);
        while (!heap_.empty()) {
            const uint32_t k = order_[pop()];
            if (!ustep(k)) continue;
            for (const auto& [j, val] : col_[us_[k].pos]) push(rank_[j]);
        }
    } else {
        for (uint32_t idx = steps; idx-- > 0;) ustep(order_[idx]);
    }
    a.swap(work_);
}

void FtFactor::btran(std::vector<double>& d, bool hyper) const {
    const uint32_t steps = static_cast<uint32_t>(us_.size());
    work_.assign(m_, 0.0);
    // U^T: increasing rank, row-wise scatter.
    auto ustep = [&](uint32_t k) {
        ++touched;
        const auto& u = us_[k];
        const double v = d[u.pos];
        if (v == 0.0) return false;
        const double z = v / u.pivot;
        work_[u.row] = z;
        for (const auto& [p, val] : u.entries) {
            ++touched;
            d[p] -= z * val;
        }
        return true;
    };
    if (hyper) {
        heap_.clear();
        APEX_FT_HEAP(true)
        for (uint32_t p = 0; p < m_; ++p)
            if (d[p] != 0.0) push(rank_[step_of_pos_[p]]);
        while (!heap_.empty()) {
            const uint32_t k = order_[pop()];
            if (!ustep(k)) continue;
            for (const auto& [p, val] : us_[k].entries) push(rank_[step_of_pos_[p]]);
        }
    } else {
        for (uint32_t idx = 0; idx < steps; ++idx) ustep(order_[idx]);
    }
    // R^T: newest first.
    for (size_t k = r_.size(); k-- > 0;) {
        const auto& e = r_[k];
        const double y = work_[e.row];
        if (y == 0.0) continue;
        for (uint32_t q = e.begin; q < e.end; ++q) {
            ++touched;
            work_[ridx_[q]] -= rval_[q] * y;
        }
    }
    // L^T: decreasing step id, scatter through the transposed copy lt_.
    auto lstep = [&](uint32_t k) {
        ++touched;
        const double y = work_[lrow_[k]];
        if (y == 0.0) return false;
        for (const auto& [j, l] : lt_[lrow_[k]]) {
            ++touched;
            work_[lrow_[j]] -= l * y;
        }
        return true;
    };
    if (hyper) {
        heap_.clear();
        APEX_FT_HEAP(false)
        for (uint32_t i = 0; i < m_; ++i)
            if (work_[i] != 0.0 && lstep_of_row_[i] >= 0) push(static_cast<uint32_t>(lstep_of_row_[i]));
        while (!heap_.empty()) {
            const uint32_t k = pop();
            if (!lstep(k)) continue;
            for (const auto& [j, l] : lt_[lrow_[k]]) push(j);
        }
    } else {
        for (uint32_t k = steps; k-- > 0;) lstep(k);
    }
    d.swap(work_);
}

#undef APEX_FT_HEAP

bool FtFactor::update(uint32_t pos, const std::vector<double>& aq) {
    const uint32_t steps = static_cast<uint32_t>(us_.size());
    // Spike in row space: R L^-1 a_q, saved by the entering column's ftran, else U * aq.
    if (spike_valid_) {
        work_.swap(spike_);
        spike_valid_ = false;
    } else {
        work_.assign(m_, 0.0);
        for (uint32_t k = 0; k < steps; ++k) {
            const auto& u = us_[k];
            double v = u.pivot * aq[u.pos];
            for (const auto& [p, val] : u.entries) v += val * aq[p];
            work_[u.row] = v;
        }
    }
    double wmax = 0;
    for (double v : work_) wmax = std::max(wmax, std::abs(v));
    const uint32_t s = step_of_pos_[pos];
    // Old column `pos` leaves U.
    for (const auto& [k, val] : col_[pos]) erase_first(us_[k].entries, pos);
    col_[pos].clear();
    // Old row of step s: scattered into val_ for the elimination, removed from the columns.
    for (const auto& [p, val] : us_[s].entries) {
        val_[p] = val;
        erase_first(col_[p], s);
    }
    us_[s].entries.clear();
    // Spike becomes column `pos`.
    for (uint32_t k = 0; k < steps; ++k) {
        if (k == s) continue;
        const double wk = work_[us_[k].row];
        if (std::abs(wk) <= 1e-14) continue;
        us_[k].entries.push_back({pos, wk});
        col_[pos].push_back({k, wk});
        ++growth_;
    }
    val_[pos] = work_[us_[s].row];
    // Step s moves to the end of the order.
    const uint32_t rs = rank_[s];
    order_.erase(order_.begin() + rs);
    order_.push_back(s);
    for (uint32_t idx = rs; idx < steps; ++idx) rank_[order_[idx]] = idx;
    // Eliminate the old row of s against the steps that now precede it: one row eta.
    const uint32_t begin = static_cast<uint32_t>(ridx_.size());
    for (uint32_t idx = rs; idx + 1 < steps; ++idx) {
        const uint32_t j = order_[idx];
        const uint32_t c = us_[j].pos;
        const double v = val_[c];
        if (v == 0.0) continue;
        val_[c] = 0.0;
        const double mult = v / us_[j].pivot;
        for (const auto& [p, val] : us_[j].entries) val_[p] -= mult * val;
        ridx_.push_back(us_[j].row);
        rval_.push_back(mult);
        ++growth_;
    }
    const double piv = val_[pos];
    val_[pos] = 0.0;
    if (ridx_.size() > begin) r_.push_back({us_[s].row, begin, static_cast<uint32_t>(ridx_.size())});
    us_[s].pivot = piv;
    ++updates_;
    return std::abs(piv) > 1e-11 * std::max(1.0, wmax);
}

}  // namespace apex
