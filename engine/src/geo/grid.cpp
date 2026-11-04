#include "geo/grid.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace apex {

SpatialGrid::SpatialGrid(const Instance& inst) : xs_(inst.xs), ys_(inst.ys), n_(inst.n) {
    if (n_ == 0) {
        cell_start_.assign(2, 0);
        return;
    }
    const auto [lx, hx] = std::minmax_element(xs_.begin(), xs_.end());
    const auto [ly, hy] = std::minmax_element(ys_.begin(), ys_.end());
    min_x_ = *lx;
    min_y_ = *ly;
    const double sx = *hx - *lx, sy = *hy - *ly;
    const double target = std::max(1.0, n_ / 2.0);  // ~2 points per cell
    if (sx > 0 && sy > 0) cell_ = std::sqrt(sx * sy / target);
    else if (sx > 0 || sy > 0) cell_ = std::max(sx, sy) / target;  // collinear
    else cell_ = 1.0;                                             // all points equal
    // Cap the grid so nx*ny stays O(n) even for very elongated boxes.
    const double cap = 4.0 * n_ + 16.0;
    while ((std::floor(sx / cell_) + 1) * (std::floor(sy / cell_) + 1) > cap) cell_ *= 1.5;
    nx_ = static_cast<uint32_t>(std::floor(sx / cell_)) + 1;
    ny_ = static_cast<uint32_t>(std::floor(sy / cell_)) + 1;

    const size_t cells = static_cast<size_t>(nx_) * ny_;
    std::vector<uint32_t> cell_of(n_);
    cell_start_.assign(cells + 1, 0);
    for (uint32_t i = 0; i < n_; ++i) {
        cell_of[i] = cell_y(ys_[i]) * nx_ + cell_x(xs_[i]);
        ++cell_start_[cell_of[i] + 1];
    }
    for (size_t c = 0; c < cells; ++c) cell_start_[c + 1] += cell_start_[c];
    items_.resize(n_);
    std::vector<uint32_t> fill(cell_start_.begin(), cell_start_.end() - 1);
    for (uint32_t i = 0; i < n_; ++i) items_[fill[cell_of[i]]++] = i;  // ascending ids per cell
}

uint32_t SpatialGrid::cell_x(double x) const {
    const double g = std::floor((x - min_x_) / cell_);
    if (!(g > 0)) return 0;
    return g >= nx_ - 1 ? nx_ - 1 : static_cast<uint32_t>(g);
}

uint32_t SpatialGrid::cell_y(double y) const {
    const double g = std::floor((y - min_y_) / cell_);
    if (!(g > 0)) return 0;
    return g >= ny_ - 1 ? ny_ - 1 : static_cast<uint32_t>(g);
}

void SpatialGrid::knn(uint32_t i, uint32_t k, std::vector<uint32_t>& out) const {
    out.clear();
    if (n_ <= 1 || k == 0) return;
    k = std::min(k, n_ - 1);

    std::vector<std::pair<double, uint32_t>> cand;  // (d2, j): total order => deterministic
    const int64_t cx = cell_x(xs_[i]), cy = cell_y(ys_[i]);
    const int64_t max_r = std::max(nx_, ny_);

    auto visit = [&](int64_t x, int64_t y) {
        if (x < 0 || y < 0 || x >= nx_ || y >= ny_) return;
        const size_t c = static_cast<size_t>(y) * nx_ + static_cast<size_t>(x);
        for (uint32_t p = cell_start_[c]; p < cell_start_[c + 1]; ++p)
            if (items_[p] != i) cand.emplace_back(d2(i, items_[p]), items_[p]);
    };

    for (int64_t r = 0; r <= max_r; ++r) {
        if (r == 0) {
            visit(cx, cy);
        } else {  // Chebyshev ring of radius r
            for (int64_t x = cx - r; x <= cx + r; ++x) { visit(x, cy - r); visit(x, cy + r); }
            for (int64_t y = cy - r + 1; y <= cy + r - 1; ++y) { visit(cx - r, y); visit(cx + r, y); }
        }
        if (cand.size() >= k) {
            std::nth_element(cand.begin(), cand.begin() + (k - 1), cand.end());
            // Any point in ring r+1 is at distance >= r * cell_ from i.
            const double bound = static_cast<double>(r) * cell_;
            if (cand[k - 1].first < bound * bound || r == max_r) break;
        }
    }
    std::sort(cand.begin(), cand.begin() + k);
    out.reserve(k);
    for (uint32_t t = 0; t < k; ++t) out.push_back(cand[t].second);
}

}  // namespace apex
