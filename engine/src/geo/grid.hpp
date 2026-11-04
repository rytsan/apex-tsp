#pragma once

#include <cstdint>
#include <vector>

#include "core/instance.hpp"

namespace apex {

// Uniform grid over the instance bounding box (square cells, ~2 points per cell).
// Distances here are squared Euclidean in double on ORIGINAL coordinates and are
// used for neighbour ranking only — never as tour costs (those use euc2d/ceil2d).
class SpatialGrid {
public:
    explicit SpatialGrid(const Instance& inst);  // O(n)

    // k nearest other nodes of i, sorted by (distance, index). Fewer if n-1 < k.
    void knn(uint32_t i, uint32_t k, std::vector<uint32_t>& out) const;

    // Calls fn(j) for every node j != i with dist(i, j) <= r.
    template <class Fn>
    void for_each_in_radius(uint32_t i, double r, Fn&& fn) const;

    uint32_t cells_x() const { return nx_; }
    uint32_t cells_y() const { return ny_; }
    double cell_size() const { return cell_; }

private:
    uint32_t cell_x(double x) const;
    uint32_t cell_y(double y) const;
    double d2(uint32_t a, uint32_t b) const {
        const double dx = xs_[a] - xs_[b], dy = ys_[a] - ys_[b];
        return dx * dx + dy * dy;
    }

    const std::vector<double>& xs_;
    const std::vector<double>& ys_;
    uint32_t n_ = 0, nx_ = 1, ny_ = 1;
    double min_x_ = 0, min_y_ = 0, cell_ = 1;
    std::vector<uint32_t> cell_start_;  // CSR: size nx*ny + 1
    std::vector<uint32_t> items_;       // node ids grouped by cell
};

template <class Fn>
void SpatialGrid::for_each_in_radius(uint32_t i, double r, Fn&& fn) const {
    if (!(r >= 0.0)) return;
    const double r2 = r * r;
    const uint32_t x0 = cell_x(xs_[i] - r), x1 = cell_x(xs_[i] + r);
    const uint32_t y0 = cell_y(ys_[i] - r), y1 = cell_y(ys_[i] + r);
    for (uint32_t cy = y0; cy <= y1; ++cy)
        for (uint32_t cx = x0; cx <= x1; ++cx) {
            const uint32_t c = cy * nx_ + cx;
            for (uint32_t p = cell_start_[c]; p < cell_start_[c + 1]; ++p) {
                const uint32_t j = items_[p];
                if (j != i && d2(i, j) <= r2) fn(j);
            }
        }
}

}  // namespace apex
