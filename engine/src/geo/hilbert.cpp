#include "geo/hilbert.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace apex {
namespace {

// Rotate/flip a quadrant of side s.
inline void rot(uint32_t s, uint32_t& u, uint32_t& v, uint32_t ru, uint32_t rv) {
    if (rv == 0) {
        if (ru == 1) {
            u = s - 1 - u;
            v = s - 1 - v;
        }
        std::swap(u, v);
    }
}

inline uint32_t to_grid(double t, double lo, double span) {
    if (!(span > 0.0)) return 0;
    const double g = (t - lo) / span * static_cast<double>(kHilbertSide);
    if (!(g > 0.0)) return 0;  // also maps NaN to 0
    return g >= static_cast<double>(kHilbertSide - 1) ? kHilbertSide - 1 : static_cast<uint32_t>(g);
}

}  // namespace

uint32_t hilbert_xy2d(uint32_t u, uint32_t v) {
    uint64_t d = 0;
    for (uint32_t s = kHilbertSide / 2; s > 0; s /= 2) {
        const uint32_t ru = (u & s) ? 1u : 0u;
        const uint32_t rv = (v & s) ? 1u : 0u;
        d += static_cast<uint64_t>(s) * s * ((3u * ru) ^ rv);
        rot(kHilbertSide, u, v, ru, rv);
    }
    return static_cast<uint32_t>(d);
}

void hilbert_d2xy(uint32_t d, uint32_t& u, uint32_t& v) {
    uint32_t t = d;
    u = v = 0;
    for (uint32_t s = 1; s < kHilbertSide; s *= 2) {
        const uint32_t ru = 1u & (t / 2);
        const uint32_t rv = 1u & (t ^ ru);
        rot(s, u, v, ru, rv);
        u += s * ru;
        v += s * rv;
        t /= 4;
    }
}

uint32_t hilbert_key(double x, double y, double min_x, double min_y, double max_x, double max_y) {
    const double span = std::max(max_x - min_x, max_y - min_y);
    return hilbert_xy2d(to_grid(x, min_x, span), to_grid(y, min_y, span));
}

std::vector<uint32_t> hilbert_order(const Instance& inst) {
    const uint32_t n = inst.n;
    std::vector<uint32_t> order(n);
    if (n == 0) return order;
    const auto [min_x, max_x] = std::minmax_element(inst.xs.begin(), inst.xs.end());
    const auto [min_y, max_y] = std::minmax_element(inst.ys.begin(), inst.ys.end());

    std::vector<uint64_t> packed(n);  // (key << 32) | index  => sort gives key order, index ties
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t key = hilbert_key(inst.xs[i], inst.ys[i], *min_x, *min_y, *max_x, *max_y);
        packed[i] = (key << 32) | i;
    }
    std::sort(packed.begin(), packed.end());
    for (uint32_t i = 0; i < n; ++i) order[i] = static_cast<uint32_t>(packed[i]);
    return order;
}

}  // namespace apex
